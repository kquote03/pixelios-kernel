/* SPDX-License-Identifier: MIT */
/*
 * libsalog.dylib: mediaserverd helper (doc 54 s7), injected with the boot-arg
 * sweetenv=mediaserverd:DYLD_INSERT_LIBRARIES=/usr/local/lib/libsalog.dylib[,SALOG_TAP_NULL=1][,SALOG_OSLOG=1] (patches 0110 + 0106).
 * SALOG_TAP_NULL=1: the audio path (VirtualAudio default route mix -> /dev/sweetaudio, see below). SALOG_OSLOG=1: interposes
 * _os_log_impl and os_log_type_enabled and prints every os_log message of the process (VirtualAudio, the HAL) to stderr,
 * which sweetnull=mediaserverd tees to the console (debug). Bounded: SALOG_MAX lines, SALOG_LINE bytes each.
 *
 * os_log buffer layout (libtrace, the public "os_log_fmt" encoding the compiler emits): byte 0 summary flags, byte 1 item count,
 * then per item: descriptor byte (high nibble type: 0 scalar, 1 count, 2 string, 3 pointer, 4 object; low nibble privacy flags),
 * size byte, size bytes of data (scalars by value, strings/objects as a pointer).
 */
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/lock.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdlib.h>
#include <os/log.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sa_core.h"
#include "sweetaudio_dev.h"

#define SALOG_MAX  30000u
#define SALOG_LINE 480

extern void _os_log_impl(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, uint32_t size);
extern bool os_log_type_enabled(os_log_t oslog, os_log_type_t type);

static _Atomic uint32_t salog_n;

struct item { int type; int size; const uint8_t *data; };

static int
items(const uint8_t *buf, uint32_t size, struct item *it, int max)
{
	if (buf == NULL || size < 2)
		return 0;
	int n = buf[1], k = 0;
	uint32_t off = 2;
	for (int i = 0; i < n && k < max && off + 2 <= size; i++) {
		int d = buf[off], s = buf[off + 1];
		if (off + 2 + (uint32_t)s > size)
			break;
		it[k].type = d >> 4;
		it[k].size = s;
		it[k].data = buf + off + 2;
		k++;
		off += 2 + (uint32_t)s;
	}
	return k;
}

static int64_t
scalar(const struct item *x)
{
	int64_t v = 0;
	if (x->size == 1) v = *(const int8_t *)x->data;
	else if (x->size == 2) { int16_t t; memcpy(&t, x->data, 2); v = t; }
	else if (x->size == 4) { int32_t t; memcpy(&t, x->data, 4); v = t; }
	else if (x->size == 8) memcpy(&v, x->data, 8);
	return v;
}

static const void *
ptr(const struct item *x)
{
	const void *p = NULL;
	if (x->size == 8)
		memcpy(&p, x->data, 8);
	return p;
}

static void
render(char *out, size_t cap, const char *fmt, const uint8_t *buf, uint32_t size)
{
	struct item it[48];
	int n = items(buf, size, it, 48), ai = 0;
	size_t o = 0;
	for (const char *p = fmt; *p && o + 1 < cap;) {
		if (*p != '%') { out[o++] = *p++; continue; }
		if (p[1] == '%') { out[o++] = '%'; p += 2; continue; }
		const char *q = p + 1;
		if (*q == '{') { while (*q && *q != '}') q++; if (*q) q++; }
		char spec[32] = "%";
		size_t sl = 1;
		int prec_star = 0;
		while (*q && strchr("-+ #0123456789.*", *q)) { if (*q == '*') prec_star = 1; else if (sl < 20) spec[sl++] = *q; q++; }
		char len[4] = "";
		size_t ll = 0;
		while (*q && strchr("hlqjztL", *q) && ll < 3) len[ll++] = *q++;
		char conv = *q ? *q++ : 0;
		p = q;
		int prec = -1;
		if (prec_star && ai < n && it[ai].type == 1) prec = (int)scalar(&it[ai++]);
		if (ai >= n) { o += (size_t)snprintf(out + o, cap - o, "<?>"); continue; }
		const struct item *x = &it[ai++];
		char tmp[256];
		tmp[0] = 0;
		if (conv == 's') {
			const char *s = ptr(x);
			snprintf(tmp, sizeof tmp, "%.*s", prec >= 0 ? prec : 200, s ? s : "(null)");
		} else if (conv == '@') {
			CFTypeRef obj = ptr(x);
			CFStringRef d = obj ? CFCopyDescription(obj) : NULL;
			if (!d || !CFStringGetCString(d, tmp, sizeof tmp, kCFStringEncodingUTF8))
				snprintf(tmp, sizeof tmp, "%s", obj ? "<obj>" : "(null)");
			if (d) CFRelease(d);
		} else if (conv == 'p') {
			snprintf(tmp, sizeof tmp, "%p", ptr(x));
		} else if (strchr("dDiuUxXoOc", conv)) {
			spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv == 'D' ? 'd' : conv == 'U' ? 'u' : conv == 'O' ? 'o' : conv; spec[sl] = 0;
			snprintf(tmp, sizeof tmp, spec, (long long)scalar(x));
		} else if (strchr("fFeEgGaA", conv)) {
			double v = 0;
			if (x->size == 8) memcpy(&v, x->data, 8);
			else if (x->size == 4) { float f; memcpy(&f, x->data, 4); v = f; }
			spec[sl++] = conv; spec[sl] = 0;
			snprintf(tmp, sizeof tmp, spec, v);
		} else {
			snprintf(tmp, sizeof tmp, "<%c>", conv ? conv : '?');
		}
		o += (size_t)snprintf(out + o, cap - o, "%s", tmp);
		if (o >= cap) o = cap - 1;
	}
	out[o < cap ? o : cap - 1] = 0;
}

static int salog_oslog = -1;
static int
oslog_on(void)
{
	if (salog_oslog < 0) {
		const char *e = getenv("SALOG_OSLOG");
		salog_oslog = e && e[0] == '1';
	}
	return salog_oslog;
}

static void
salog_os_log_impl(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, uint32_t size)
{
	if (format && oslog_on() && atomic_fetch_add(&salog_n, 1) < SALOG_MAX) {
		char line[SALOG_LINE];
		int h = snprintf(line, sizeof line, "valog[%d] t%d: ", getpid(), (int)type);
		render(line + h, sizeof line - (size_t)h - 1, format, buf, size);
		size_t l = strlen(line);
		line[l++] = '\n';
		(void)write(2, line, l);
	}
	_os_log_impl(dso, log, type, format, buf, size);
}

/* (run a12l: hiding Null_Device from 'dev#' reads had no effect; VirtualAudio gets its device list inside CoreAudio.) */
/* the device VirtualAudio actually runs IO on: its HAL aggregate ("VAD [vdef] AggDev 1", transport grup; run a12m: no IOProc is ever
 * created on Null_Device itself, the aggregate holds it as sub-device) */
static int
is_tap_device(AudioObjectID id)
{
	AudioObjectPropertyAddress u = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	CFStringRef s = NULL;
	UInt32 sz = sizeof s;
	if (AudioObjectGetPropertyData(id, &u, 0, NULL, &sz, &s) != 0 || s == NULL)
		return 0;
	int r = CFStringHasPrefix(s, CFSTR("VAD [")) || CFStringCompare(s, CFSTR("Null_Device"), 0) == kCFCompareEqualTo;
	CFRelease(s);
	return r;
}

/* SALOG_TAP_NULL=1 (doc 54 s7, doc 55): VirtualAudio keeps its default route on Null_Device (runs a12i-l) and drives it through a HAL
 * aggregate ('VAD [vdef] AggDev 1') with the public AudioDeviceCreateIOProcID/AudioDeviceStart. Every IOProc created on such a device is
 * wrapped: VirtualAudio's proc runs, then its final mix (Float32, after VirtualAudio's volume) is resampled linearly from the device's
 * nominal rate to 48 kHz into a per-tap buffer. Polish pass (a12p):
 *  - mix: the taps add into one accumulator (sa_mix, summing where concurrent streams overlap); the leader tap (the last one called
 *    within TAP_LEAD_MS) flushes it, so two VirtualAudio streams sum instead of interleaving in the kernel FIFO;
 *  - loudness: make-up gain SALOG_GAIN_DB (default 12 dB, 0..24) with a block peak limiter at TAP_CEIL (-1 dBFS) before S16; the kernel then
 *    applies its own gain (default -6 dB) and its -3 dBFS sample clamp (patch 0394);
 *  - drift: before each write the kernel FIFO fill (SWAU_IOC_POS) trims the resample ratio (sa_drift_adj, <= 0.3 %) towards TAP_FILL frames;
 *  - evidence: one "tap: sound" line per sound (after 1 s of silence): frames, input peak, output peak, gain, fill, drift, kernel counters.
 * The fd stays open (the kernel keeps the amps on while a writer is attached). IO thread: one os_unfair_lock, no allocation. */
#define TAP_MAX      8u
#define TAP_LEAD_MS  50u
#define TAP_CEIL     0.891f          /* -1 dBFS */
#define TAP_REL      1.03f           /* limiter release per IO block (~0.26 dB) */
#define TAP_FILL     1200.0          /* frames queued in the kernel FIFO before a write (25 ms) */
struct tap {
	AudioDeviceIOProc proc;
	void *ctx;
	double step;          /* input frames per output frame (nominal) */
	double pos;           /* fractional read position into [last, buf...] */
	float last[2];
	uint32_t off;         /* this tap's position in the mix accumulator */
	uint64_t in_frames, calls;
	float buf[8192 * 2];
};
static int salog_tap = -1, tap_fd = -1;
static os_unfair_lock tap_lk = OS_UNFAIR_LOCK_INIT;
static struct tap *taps[TAP_MAX];
static uint32_t *tap_offs[TAP_MAX], ntaps;
static struct sa_mix tap_mix;
static struct sa_lim tap_lim;
static struct tap *tap_leader;
static uint64_t tap_leader_t, tap_lead_ticks, tap_out_frames, tap_dropped, tap_flushes;
static double tap_drift, tap_drift_avg;
static int16_t tap_out[SA_MIXN * 2];
static struct { uint64_t frames, silent; float pk_in, gmin; int32_t pk_out; uint32_t fill; int on; } snd;

static int
tap_enabled(void)
{
	if (salog_tap < 0) {
		const char *e = getenv("SALOG_TAP_NULL");
		salog_tap = e && e[0] == '1';
	}
	return salog_tap;
}

/* the leader's flush: n frames of the accumulator -> limiter -> S16 -> /dev/sweetaudio (non-blocking; what does not fit is dropped) */
static void
tap_flush(uint32_t n)
{
	float pk = 0;
	for (uint32_t i = 0; i < 2 * n; i++) {
		float a = fabsf(tap_mix.acc[i]);
		pk = a > pk ? a : pk;
	}
	float g = sa_lim_block(&tap_lim, pk);
	int32_t pko = 0;
	for (uint32_t i = 0; i < 2 * n; i++) {
		tap_out[i] = sa_f2s16(tap_mix.acc[i] * g);
		int32_t a = tap_out[i] < 0 ? -tap_out[i] : tap_out[i];
		pko = a > pko ? a : pko;
	}
	if (tap_fd < 0)
		tap_fd = open("/dev/sweetaudio", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	struct swau_pos ps = { 0 };
	if (tap_fd >= 0 && ioctl(tap_fd, SWAU_IOC_POS, &ps) == 0)
		tap_drift = sa_drift_adj(&tap_drift_avg, ps.fifo_fill, TAP_FILL, 2e-6, 0.003);
	if (tap_fd >= 0 && n > 0) {
		uint32_t took = sa_frames_taken(write(tap_fd, tap_out, (size_t)n * 4), n);
		tap_out_frames += took;
		tap_dropped += n - took;
	}
	tap_flushes++;
	if (pk > 1e-4f) {                 /* a sound: count it */
		if (!snd.on)
			snd.gmin = g;
		snd.on = 1;
		snd.frames += n;
		snd.silent = 0;
		snd.pk_in = pk > snd.pk_in ? pk : snd.pk_in;
		snd.pk_out = pko > snd.pk_out ? pko : snd.pk_out;
		snd.gmin = g < snd.gmin ? g : snd.gmin;
		snd.fill = ps.fifo_fill;
	} else if (snd.on && (snd.silent += n) >= 48000) {
		char m[300];
		int l = snprintf(m, sizeof m, "valog[%d]: tap: sound: %llu frames, peak in %d out %d (s16, kernel gain follows), gain min %.1f dB, fill %u, "
		    "drift %d ppm, kernel underruns %u overruns %u, taps %u, written %llu dropped %llu\n", getpid(), (unsigned long long)snd.frames,
		    (int)(snd.pk_in * 32767.0f), (int)snd.pk_out, 20.0 * log10((double)snd.gmin), snd.fill, (int)(tap_drift * 1e6), ps.underruns,
		    ps.overruns, ntaps, (unsigned long long)tap_out_frames, (unsigned long long)tap_dropped);
		(void)write(2, m, (size_t)l);
		memset(&snd, 0, sizeof snd);
	}
}

static OSStatus
tap_proc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *it, AudioBufferList *out,
    const AudioTimeStamp *ot, void *ctx)
{
	struct tap *t = ctx;
	OSStatus r = t->proc(dev, now, in, it, out, ot, t->ctx);
	if (out == NULL || out->mNumberBuffers == 0 || out->mBuffers[0].mData == NULL)
		return r;
	const AudioBuffer *b0 = &out->mBuffers[0], *b1 = out->mNumberBuffers > 1 ? &out->mBuffers[1] : NULL;
	UInt32 ch = b0->mNumberChannels ? b0->mNumberChannels : 1;
	UInt32 n = b0->mDataByteSize / (4u * ch);
	const float *p0 = b0->mData, *p1 = (b1 && b1->mData) ? b1->mData : NULL;
	/* frame i, channel c (interleaved in buffer 0, or planar over buffers 0/1) */
#define SMP(i, c) (ch >= 2 ? p0[(size_t)(i) * ch + (c)] : ((c) && p1) ? p1[(i)] : p0[(size_t)(i) * ch])
	double step = t->step * (1.0 + tap_drift);
	UInt32 k = 0;
	while (k < 8192) {
		double x = t->pos;            /* position in input frames, -1 .. n-1 (-1 = t->last) */
		long i0 = (long)(x < 0 ? -1 : (long)x);
		if (i0 + 1 >= (long)n)
			break;
		float f = (float)(x - (double)i0);
		for (int c = 0; c < 2; c++) {
			float a = i0 < 0 ? t->last[c] : SMP(i0, c), b = SMP(i0 + 1, c);
			t->buf[2 * k + c] = a + f * (b - a);
		}
		k++;
		t->pos += step;
	}
	if (n > 0) {
		t->last[0] = SMP(n - 1, 0);
		t->last[1] = SMP(n - 1, 1);
	}
#undef SMP
	t->pos -= (double)n;      /* next buffer: index -1 is this buffer's last frame */
	t->in_frames += n;
	t->calls++;
	os_unfair_lock_lock(&tap_lk);
	uint64_t tn = mach_absolute_time();
	if (tap_leader == NULL || tap_leader == t || tn - tap_leader_t > tap_lead_ticks) {
		tap_leader = t;
		tap_leader_t = tn;
	}
	tap_dropped += k - sa_mix_add(&tap_mix, &t->off, t->buf, k);
	if (tap_leader == t) {
		uint32_t fl = t->off;
		tap_flush(fl);
		sa_mix_take(&tap_mix, fl, tap_offs, ntaps);
	}
	uint64_t calls = t->calls, outf = tap_out_frames, drop = tap_dropped;
	int fd = tap_fd, ppm = (int)(tap_drift * 1e6);
	os_unfair_lock_unlock(&tap_lk);
	if (calls == 1 || calls % 2000 == 0) {
		char m[220];
		int l = snprintf(m, sizeof m, "valog[%d]: tap: call %llu, in %llu frames (%u ch, %u bufs), out %llu, dropped %llu, drift %d ppm, fd %d\n",
		    getpid(), (unsigned long long)calls, (unsigned long long)t->in_frames, (unsigned)ch, (unsigned)out->mNumberBuffers,
		    (unsigned long long)outf, (unsigned long long)drop, ppm, fd);
		(void)write(2, m, (size_t)l);
	}
	return r;
}

static OSStatus
salog_create_ioproc(AudioObjectID dev, AudioDeviceIOProc proc, void *ctx, AudioDeviceIOProcID *out)
{
	if (!tap_enabled() || proc == NULL || ntaps >= TAP_MAX || !is_tap_device(dev))
		return AudioDeviceCreateIOProcID(dev, proc, ctx, out);
	struct tap *t = calloc(1, sizeof *t);
	if (t == NULL)
		return AudioDeviceCreateIOProcID(dev, proc, ctx, out);
	Float64 rate = 0;
	UInt32 sz = sizeof rate;
	AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &rate) != 0 || rate < 8000 || rate > 192000)
		rate = 48000;
	t->proc = proc;
	t->ctx = ctx;
	t->step = rate / 48000.0;
	t->pos = 0;
	float gdb = 12.0f;
	const char *e = getenv("SALOG_GAIN_DB");
	if (e != NULL)
		gdb = (float)atof(e);
	gdb = gdb < 0.0f ? 0.0f : gdb > 24.0f ? 24.0f : gdb;
	OSStatus r = AudioDeviceCreateIOProcID(dev, tap_proc, t, out);
	os_unfair_lock_lock(&tap_lk);
	if (r == 0) {
		if (ntaps == 0) {
			mach_timebase_info_data_t tb;
			mach_timebase_info(&tb);
			tap_lead_ticks = (uint64_t)TAP_LEAD_MS * 1000000u * tb.denom / tb.numer;
			sa_lim_init(&tap_lim, powf(10.0f, gdb / 20.0f), TAP_CEIL, TAP_REL);
		}
		taps[ntaps] = t;
		tap_offs[ntaps] = &t->off;
		ntaps++;
	}
	unsigned nt = ntaps;
	os_unfair_lock_unlock(&tap_lk);
	char m[200];
	int l = snprintf(m, sizeof m, "valog[%d]: tap: wrapped IOProc on device %u (rate %.0f -> 48000), status %d, taps %u, make-up %.1f dB, limit -1 dBFS\n",
	    getpid(), (unsigned)dev, rate, (int)r, nt, gdb);
	(void)write(2, m, (size_t)l);
	if (r != 0)
		free(t);
	return r;
}

static bool
salog_type_enabled(os_log_t log, os_log_type_t type)
{
	return oslog_on() ? true : os_log_type_enabled(log, type);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } salog_interpose[] = {
	{ (const void *)salog_os_log_impl, (const void *)_os_log_impl },
	{ (const void *)salog_type_enabled, (const void *)os_log_type_enabled },
	{ (const void *)salog_create_ioproc, (const void *)AudioDeviceCreateIOProcID },
};

__attribute__((constructor)) static void
salog_init(void)
{
	char m[64];
	int l = snprintf(m, sizeof m, "valog[%d]: libsalog loaded\n", getpid());
	(void)write(2, m, (size_t)l);
}
