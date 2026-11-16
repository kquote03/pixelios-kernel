/*
 * sweetgl_once: G5 launcher for sweetgl_selftest (docs/research/52 s.12). Started once per boot by a launchd job.
 * usage: sweetgl_once SELFTEST DYLIB
 *
 * Rules (tools/test_sweetgl_once.py):
 * 1. A try whose result line is "sweetgl selftest: clear=SKIP tri=SKIP" and that printed no "GL_RENDERER=" line ran no GPU work
 *    (the SweetMali service was not there yet): wait and try again.
 * 2. Any other outcome (PASS/FAIL, a renderer line, no result line, a crash, a timeout) ends the tries: GPU work runs at most once.
 * 3. At most MAX tries; every try is bounded in time (the child is killed) and in forwarded bytes; exit 0 only for a PASS.
 * 4. Every child line (stdout and stderr) goes to the log sink: /dev/null on the phone, which xnu patch 0048 copies into the
 *    kernel log for the process names in sweetnull=.
 * No GPU, no IOKit, nothing privileged here: fork, exec, read a pipe, write lines.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LINE_MAX_ONCE   512             /* a longer line is forwarded in pieces */
#define FWD_MAX         (64 * 1024)     /* forwarded bytes per try; the rest is read and parsed but not forwarded */
#define DEF_TRIES       30
#define DEF_DELAY_MS    2000
#define DEF_TRY_MS      30000

static int sink = -1;

static void
out(const char *s, size_t n)
{
	while (n > 0 && sink >= 0) {
		ssize_t w = write(sink, s, n);

		if (w <= 0) {
			if (w < 0 && errno == EINTR) {
				continue;
			}
			return;
		}
		s += w;
		n -= (size_t)w;
	}
}

static void
say(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (n > 0) {
		out(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
	}
}

static long
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static long
knob(const char *name, long def, long lo, long hi)
{
#ifdef SWEETGL_ONCE_TEST                /* the phone build takes no settings from the environment */
	const char *v = getenv(name);
	char *e = NULL;
	long x;

	if (v != NULL && *v != 0) {
		x = strtol(v, &e, 10);
		if (e != NULL && *e == 0 && x >= lo && x <= hi) {
			return x;
		}
	}
#else
	(void)name; (void)lo; (void)hi;
#endif
	return def;
}

struct seen { int renderer, skip, pass, result; };

static void
line(struct seen *s, const char *l, size_t n, size_t *fwd)
{
	static const char R[] = "sweetgl selftest: clear=";

	if (*fwd < FWD_MAX) {
		out(l, n);
		out("\n", 1);
		*fwd += n + 1;
		if (*fwd >= FWD_MAX) {
			say("sweetgl_once: output cut after %d bytes\n", FWD_MAX);
		}
	}
	if (n >= 12 && memmem(l, n, "GL_RENDERER=", 12) != NULL) {
		s->renderer = 1;
	}
	if (n >= sizeof R - 1 && memcmp(l, R, sizeof R - 1) == 0) {     /* the last result line counts */
		s->result = 1;
		s->skip = n == 37 && memcmp(l, "sweetgl selftest: clear=SKIP tri=SKIP", 37) == 0;
		s->pass = n == 37 && memcmp(l, "sweetgl selftest: clear=PASS tri=PASS", 37) == 0;
	}
}

/* one try: 0..255 = the child's exit status, 256 + sig = killed by a signal, -1 = could not start, -2 = timed out (killed) */
static int
try_once(char *const argv[], long try_ms, struct seen *s)
{
	char buf[LINE_MAX_ONCE], rd[1024];
	size_t len = 0, fwd = 0;
	int p[2], st = 0, timed = 0;
	long end = now_ms() + try_ms, left;
	pid_t pid;
	ssize_t r, i;

	memset(s, 0, sizeof *s);
	if (pipe(p) != 0) {
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		if (sink > 2) {
			close(sink);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	close(p[1]);
	for (;;) {
		struct pollfd pf = { p[0], POLLIN, 0 };

		left = end - now_ms();
		if (left <= 0) {
			timed = 1;
			break;
		}
		if (poll(&pf, 1, (int)(left > 1000 ? 1000 : left)) <= 0) {
			continue;
		}
		r = read(p[0], rd, sizeof rd);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			break;                  /* end of file: the child (and whatever it started) closed the pipe */
		}
		for (i = 0; i < r; i++) {
			if (rd[i] == '\n' || len == sizeof buf) {
				line(s, buf, len, &fwd);
				len = 0;
				if (rd[i] == '\n') {
					continue;
				}
			}
			buf[len++] = rd[i];
		}
	}
	if (len > 0) {
		line(s, buf, len, &fwd);
	}
	close(p[0]);
	if (timed) {
		kill(pid, SIGKILL);
	}
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
	}
	if (timed) {
		return -2;
	}
	return WIFEXITED(st) ? WEXITSTATUS(st) : 256 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

int
main(int argc, char **argv)
{
	long tries = knob("SWEETGL_ONCE_MAX", DEF_TRIES, 1, 1000), delay_ms = knob("SWEETGL_ONCE_DELAY_MS", DEF_DELAY_MS, 0, 600000);
	long try_ms = knob("SWEETGL_ONCE_TRY_MS", DEF_TRY_MS, 100, 600000), n;
	const char *log = "/dev/null";
	char *cargv[3];
	struct seen s;
	int rc;

	if (argc != 3) {
		fprintf(stderr, "usage: sweetgl_once SELFTEST DYLIB\n");
		return 64;
	}
#ifdef SWEETGL_ONCE_TEST
	if (getenv("SWEETGL_ONCE_LOG") != NULL) {
		log = getenv("SWEETGL_ONCE_LOG");
	}
#endif
	sink = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
	signal(SIGPIPE, SIG_IGN);
	cargv[0] = argv[1];
	cargv[1] = argv[2];
	cargv[2] = NULL;
	for (n = 1; n <= tries; n++) {
		rc = try_once(cargv, try_ms, &s);
		if (rc == -2) {
			say("sweetgl_once: try %ld timed out after %ld ms, child killed; not tried again\n", n, try_ms);
			return 3;
		}
		if (rc == -1) {
			say("sweetgl_once: try %ld could not start the self-test\n", n);
			return 5;
		}
		if (rc < 256 && s.result && s.skip && !s.renderer) {    /* rule 1: no GPU work ran */
			if (n < tries) {
				struct timespec t = { delay_ms / 1000, (delay_ms % 1000) * 1000000L };

				nanosleep(&t, NULL);
			}
			continue;
		}
		say("sweetgl_once: done after %ld %s, status %d%s\n", n, n == 1 ? "try" : "tries", rc, s.result ? "" : " (no result line)");
		if (rc >= 256) {
			return 128 + (rc - 256);
		}
		if (rc == 0 && !(s.result && s.pass)) {
			return 4;               /* exit 0 without a PASS line is not a pass */
		}
		return rc;
	}
	say("sweetgl_once: gave up after %ld tries (the SweetMali service never appeared)\n", tries);
	return 2;
}
