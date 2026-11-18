/* SPDX-License-Identifier: MIT */
/*
 * libsalog.dylib: debug-only visibility for mediaserverd (doc 54 s7). Injected with the boot-arg
 * sweetenv=mediaserverd:DYLD_INSERT_LIBRARIES=/usr/local/lib/libsalog.dylib (patches 0110 + 0106), it interposes
 * _os_log_impl and os_log_type_enabled and prints every os_log message of the process (VirtualAudio, the HAL) to stderr,
 * which sweetnull=mediaserverd tees to the console. Bounded: SALOG_MAX lines, SALOG_LINE bytes each.
 *
 * os_log buffer layout (libtrace, the public "os_log_fmt" encoding the compiler emits): byte 0 summary flags, byte 1 item count,
 * then per item: descriptor byte (high nibble type: 0 scalar, 1 count, 2 string, 3 pointer, 4 object; low nibble privacy flags),
 * size byte, size bytes of data (scalars by value, strings/objects as a pointer).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static void
salog_os_log_impl(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, uint32_t size)
{
	if (format && atomic_fetch_add(&salog_n, 1) < SALOG_MAX) {
		char line[SALOG_LINE];
		int h = snprintf(line, sizeof line, "valog[%d] t%d: ", getpid(), (int)type);
		render(line + h, sizeof line - (size_t)h - 1, format, buf, size);
		size_t l = strlen(line);
		line[l++] = '\n';
		(void)write(2, line, l);
	}
	_os_log_impl(dso, log, type, format, buf, size);
}

static bool
salog_type_enabled(os_log_t log, os_log_type_t type)
{
	(void)log; (void)type;
	return true;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } salog_interpose[] = {
	{ (const void *)salog_os_log_impl, (const void *)_os_log_impl },
	{ (const void *)salog_type_enabled, (const void *)os_log_type_enabled },
};

__attribute__((constructor)) static void
salog_init(void)
{
	char m[64];
	int l = snprintf(m, sizeof m, "valog[%d]: libsalog loaded\n", getpid());
	(void)write(2, m, (size_t)l);
}
