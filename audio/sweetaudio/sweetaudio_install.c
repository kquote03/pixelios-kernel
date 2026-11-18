/* SPDX-License-Identifier: MIT */
/*
 * sweetaudio_install [MODE]: run once at boot by the launchd job org.pixelios.sweetaudio (dev overlay; doc 54 s.6).
 * The overlay may only write /usr/local, so the bundle is staged at /usr/local/pixos/SweetAudio.driver and copied here
 * into the HAL plug-in folder that mediaserverd scans (/Library/Audio/Plug-Ins/HAL).
 *   driver     install SweetAudio.driver; leave VirtualAudio where it is
 *   driver+va  install SweetAudio.driver and move VirtualAudio.plugin back from HAL.disabled to HAL
 *   restore    remove SweetAudio.driver; move VirtualAudio.plugin back to HAL.disabled (the root-tweak state)
 *   status     change nothing, print the state
 * Without MODE the first word of /usr/local/pixos/sweetaudio.mode is used (so one launchd job serves every mode and the
 * overlay picks the mode by that file). A second word "tone" in that file then waits (<= 300 s) for /dev/sweetaudio and runs /usr/local/bin/sweetaudio_tone 3 880 3
 * (3 beeps, kernel path without CoreAudio; bounded to 5 s); "play" runs /usr/local/bin/sweetaudio_play both (HAL + AudioQueue).
 * When something changed, mediaserverd is killed once so launchd restarts it and the HAL rescans (KeepAlive job).
 * Every step prints one "sweetaudio_install: ..." line. Exit 0 ok, 1 a step failed, 2 usage. Bounded: fixed file list.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define SRC   "/usr/local/pixos/SweetAudio.driver"
#define HAL   "/Library/Audio/Plug-Ins/HAL"
#define DST   HAL "/SweetAudio.driver"
#define VA_ON HAL "/VirtualAudio.plugin"
#define VA_OFF "/Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin"

static const struct { const char *name; mode_t mode; } files[] = { { "Info.plist", 0644 }, { "SweetAudio", 0755 } };
static int changed, failed;

static int
same(const char *a, const char *b)
{
	int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY), eq = fa >= 0 && fb >= 0;
	char x[16384], y[16384];
	while (eq) {
		ssize_t na = read(fa, x, sizeof x), nb = read(fb, y, sizeof y);
		if (na != nb || na < 0 || memcmp(x, y, (size_t)na) != 0)
			eq = 0;
		if (na <= 0)
			break;
	}
	if (fa >= 0)
		close(fa);
	if (fb >= 0)
		close(fb);
	return eq;
}

static int
copy(const char *from, const char *to, mode_t mode)
{
	char tmp[256];
	snprintf(tmp, sizeof tmp, "%s.sweettmp", to);
	int in = open(from, O_RDONLY), out = in >= 0 ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode) : -1, ok = in >= 0 && out >= 0;
	char buf[65536];
	for (ssize_t n; ok && (n = read(in, buf, sizeof buf)) != 0;)
		ok = n > 0 && write(out, buf, (size_t)n) == n;
	if (out >= 0) {
		ok = ok && fchmod(out, mode) == 0 && fchown(out, 0, 0) == 0 && fsync(out) == 0;
		close(out);
	}
	if (in >= 0)
		close(in);
	ok = ok && rename(tmp, to) == 0;
	if (!ok) {
		printf("sweetaudio_install: FAIL copy %s -> %s errno %d\n", from, to, errno);
		unlink(tmp);
	}
	return ok;
}

static void
mkdir1(const char *d)
{
	if (mkdir(d, 0755) == 0) {
		printf("sweetaudio_install: made %s\n", d);
		changed = 1;
	} else if (errno != EEXIST) {
		printf("sweetaudio_install: FAIL mkdir %s errno %d\n", d, errno);
		failed = 1;
	}
}

static void
install_driver(void)
{
	char s[256], d[256];
	mkdir1(HAL);
	mkdir1(DST);   /* flat iOS bundle, like the root's own .driver bundles: Info.plist and the binary at the top */
	for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
		snprintf(s, sizeof s, "%s/%s", SRC, files[i].name);
		snprintf(d, sizeof d, "%s/%s", DST, files[i].name);
		if (access(s, R_OK) != 0) {
			printf("sweetaudio_install: FAIL missing %s\n", s);
			failed = 1;
		} else if (same(s, d)) {
			printf("sweetaudio_install: same %s\n", d);
		} else if (copy(s, d, files[i].mode)) {
			printf("sweetaudio_install: wrote %s\n", d);
			changed = 1;
		} else {
			failed = 1;
		}
	}
}

static void
remove_driver(void)
{
	static const char *paths[] = { DST "/SweetAudio", DST "/Info.plist", DST };
	for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
		int r = i < 2 ? unlink(paths[i]) : rmdir(paths[i]);
		if (r == 0) {
			printf("sweetaudio_install: removed %s\n", paths[i]);
			changed = 1;
		} else if (errno != ENOENT) {
			printf("sweetaudio_install: FAIL remove %s errno %d\n", paths[i], errno);
			failed = 1;
		}
	}
}

static void
move_dir(const char *from, const char *to)
{
	struct stat st;
	if (lstat(to, &st) == 0) {
		printf("sweetaudio_install: same %s (present)\n", to);
		return;
	}
	if (lstat(from, &st) != 0) {
		printf("sweetaudio_install: FAIL neither %s nor %s present\n", from, to);
		failed = 1;
		return;
	}
	if (rename(from, to) == 0) {
		printf("sweetaudio_install: moved %s -> %s\n", from, to);
		changed = 1;
	} else {
		printf("sweetaudio_install: FAIL move %s -> %s errno %d\n", from, to, errno);
		failed = 1;
	}
}

static void
kick_mediaserverd(void)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
	size_t len = 0;
	if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0 || len == 0 || len > (64u << 20))
		return;
	len += len / 4;
	struct kinfo_proc *kp = malloc(len);
	if (kp == NULL || sysctl(mib, 3, kp, &len, NULL, 0) != 0) {
		free(kp);
		return;
	}
	for (size_t i = 0; i < len / sizeof *kp; i++) {
		if (strcmp(kp[i].kp_proc.p_comm, "mediaserverd") == 0) {
			int r = kill(kp[i].kp_proc.p_pid, SIGKILL);
			printf("sweetaudio_install: kicked mediaserverd pid %d (%s)\n", kp[i].kp_proc.p_pid, r == 0 ? "ok" : strerror(errno));
		}
	}
	free(kp);
}

/* the kernel makes /dev/sweetaudio once the AOC + amps are up (~+120 s, patch 0173) and holds it for its 2-beep self-test: wait for an
 * open to succeed, bounded to 300 s (1 s steps) */
static int
wait_dev(void)
{
	for (int s = 0; s < 300; s++) {
		int fd = open("/dev/sweetaudio", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0) {
			close(fd);
			printf("sweetaudio_install: /dev/sweetaudio ready after %d s\n", s);
			return 1;
		}
		if (s % 30 == 0)
			printf("sweetaudio_install: waiting for /dev/sweetaudio (errno %d)\n", errno);
		sleep(1);
	}
	printf("sweetaudio_install: FAIL /dev/sweetaudio not ready after 300 s\n");
	return 0;
}

static void
status(void)
{
	struct stat st;
	printf("sweetaudio_install: state: driver %s, VirtualAudio %s, /dev/sweetaudio %s\n",
	    stat(DST "/SweetAudio", &st) == 0 ? "installed" : "absent",
	    lstat(VA_ON, &st) == 0 ? "enabled" : lstat(VA_OFF, &st) == 0 ? "disabled" : "missing",
	    stat("/dev/sweetaudio", &st) == 0 ? "present" : "absent");
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	char mbuf[32] = "", extra[32] = "";
	const char *m = argc == 2 ? argv[1] : mbuf;
	if (argc == 1) {
		FILE *f = fopen("/usr/local/pixos/sweetaudio.mode", "r");
		if (f == NULL || fscanf(f, "%31s %31s", mbuf, extra) < 1)
			printf("sweetaudio_install: FAIL no mode (argument or /usr/local/pixos/sweetaudio.mode)\n");
		if (f)
			fclose(f);
	}
	if (strcmp(m, "driver") == 0) {
		install_driver();
	} else if (strcmp(m, "driver+va") == 0) {
		install_driver();
		move_dir(VA_OFF, VA_ON);
	} else if (strcmp(m, "restore") == 0) {
		remove_driver();
		mkdir1("/Library/Audio/Plug-Ins/HAL.disabled");
		move_dir(VA_ON, VA_OFF);
	} else if (strcmp(m, "status") != 0) {
		fprintf(stderr, "usage: sweetaudio_install driver|driver+va|restore|status\n");
		return 2;
	}
	if (changed)
		kick_mediaserverd();
	status();
	printf("sweetaudio_install: done %s: %s, %s\n", m, changed ? "changed" : "unchanged", failed ? "FAIL" : "ok");
	if ((strcmp(extra, "tone") == 0 || strcmp(extra, "play") == 0) && wait_dev()) {
		extern char **environ;
		char *tone_argv[] = { "/usr/local/bin/sweetaudio_tone", "3", "880", "3", NULL };
		char *play_argv[] = { "/usr/local/bin/sweetaudio_play", "both", NULL };
		char **targv = strcmp(extra, "tone") == 0 ? tone_argv : play_argv;
		pid_t pid;
		int st = 0, r = posix_spawn(&pid, targv[0], NULL, NULL, targv, environ);
		if (r == 0 && waitpid(pid, &st, 0) == pid)
			printf("sweetaudio_install: %s exit %d\n", extra, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		else
			printf("sweetaudio_install: FAIL %s spawn %d\n", extra, r);
	}
	return failed ? 1 : 0;
}
