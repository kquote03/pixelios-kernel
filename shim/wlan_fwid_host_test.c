/* Host test of xnu patch 0396 (oriole firmware identity): sweet_wlan_wr.h as build.sh assembles it through patch 0174 (the header upstream's
 * wlan_wr_host_test.c checks with bluejay's pinned values) plus the sweet_wlan_wr.h hunk of patch 0396 on top. Checks the oriole values: the row
 * chosen by the ADT model-number bytes, the raised image bound, and swl_fw_pin accepting only the chosen row (no bus access). Nothing here touches hardware. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned fails;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)

static char logbuf[1 << 14];
static unsigned loglen;
static int cap_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int cap_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(logbuf + loglen, sizeof logbuf - loglen, fmt, ap);
	va_end(ap);
	if (n > 0 && loglen + (unsigned)n < sizeof logbuf)
		loglen += (unsigned)n;
	return n;
}
#define printf cap_printf
#include "sweet_wlan_chip.h"
#include "sweet_wlan_wr.h"
#undef printf

static unsigned nbus;
static int f_rd(void *c, int s, uint32_t w, uint32_t o, uint32_t *v) { (void)c; (void)s; (void)w; (void)o; (void)v; nbus++; return 1; }
static int f_wr(void *c, int s, uint32_t w, uint32_t o, uint32_t v) { (void)c; (void)s; (void)w; (void)o; (void)v; nbus++; return 1; }
static int f_map2(void *c) { (void)c; nbus++; return 1; }
static int f_rng(void *c, uint8_t *b, uint32_t n) { (void)c; (void)b; (void)n; nbus++; return 1; }
static void f_delay(void *c, unsigned us) { (void)c; (void)us; nbus++; }
static const struct swl_wbus bus = { f_rd, f_wr, f_map2, f_rng, f_delay, 0 };

/* swl_fw_pin with the board row `id` against an image of (len, crc): its return value; x is fresh each time */
static int pin(const struct swl_fwid *id, uint32_t len, uint32_t crc, int *stopped)
{
	struct swl_wx x;
	int r;

	memset(&x, 0, sizeof x);
	x.b = &bus;
	x.row = 14;
	if (id) {
		x.pin_len = id->len;
		x.pin_crc = id->crc;
	}
	x.img_len = len;
	x.img_crc = crc;
	r = swl_fw_pin(&x);
	*stopped = x.stopped;
	return r;
}

int main(void)
{
	const struct swl_fwid *bj = swl_fw_ident("bluejay", 8), *or = swl_fw_ident("oriole", 7);
	int st;

	/* the constants: bluejay's pin unchanged, oriole's row, the bound raised by exactly the 73 B oriole needs */
	CHECK(SWW_FW_LEN == 1764018u && SWW_FW_CRC == 0x1153ccffu);
	CHECK(SWW_FW_LEN_ORIOLE == 1764091u && SWW_FW_CRC_ORIOLE == 0x92567396u);
	CHECK(SWW_RAM_MIN == 1791299u && SWW_IMG_MAX == 1764091u && SWW_IMG_MAXW == 441023u && SWW_IMG_END == 0x3AEAFCu && SWW_IMG_END <= SWW_WIN_TAIL);
	CHECK(SWW_RAM_MIN - 1791226u == SWW_FW_LEN_ORIOLE - SWW_FW_LEN);
	CHECK(2752512u >= SWW_RAM_MIN);         /* oriole RAM observed (run wlan-oriole-l3) */
	CHECK(sizeof swl_fwids / sizeof swl_fwids[0] == 2u);

	/* the row choice: exact model bytes incl. the NUL (SecureDTGetProperty size), everything else is row 0 = bluejay */
	CHECK(bj == &swl_fwids[0] && bj->len == SWW_FW_LEN && bj->crc == SWW_FW_CRC && strcmp(bj->model, "bluejay") == 0);
	CHECK(or == &swl_fwids[1] && or->len == SWW_FW_LEN_ORIOLE && or->crc == SWW_FW_CRC_ORIOLE);
	CHECK(swl_fw_ident(0, 0) == &swl_fwids[0]);
	CHECK(swl_fw_ident(0, 7) == &swl_fwids[0]);
	CHECK(swl_fw_ident("oriole", 6) == &swl_fwids[0]);              /* size without the NUL */
	CHECK(swl_fw_ident("oriole\0x", 8) == &swl_fwids[0]);           /* size too large */
	CHECK(swl_fw_ident("orioleX", 8) == &swl_fwids[0]);
	CHECK(swl_fw_ident("oriol", 6) == &swl_fwids[0]);
	CHECK(swl_fw_ident("Oriole", 7) == &swl_fwids[0]);
	CHECK(swl_fw_ident("raven", 6) == &swl_fwids[0]);
	CHECK(swl_fw_ident("", 1) == &swl_fwids[0]);

	/* swl_fw_pin: only the chosen row passes; no bus access in any case (armed is 0) */
	CHECK(pin(or, 1764091u, 0x92567396u, &st) == 0 && !st);
	CHECK(pin(bj, 1764018u, 0x1153ccffu, &st) == 0 && !st);
	loglen = 0;
	CHECK(pin(or, 1764018u, 0x1153ccffu, &st) == -SWW_E_IMG && st);         /* bluejay's image on oriole */
	CHECK(strstr(logbuf, "W14 firmware identity 1764018 bytes crc 0x1153ccff is not the pinned 1764091 bytes crc 0x92567396") != 0);
	CHECK(pin(bj, 1764091u, 0x92567396u, &st) == -SWW_E_IMG && st);         /* oriole's image on bluejay */
	CHECK(pin(or, 1764091u, 0x1153ccffu, &st) == -SWW_E_IMG && st);         /* length of one row, CRC of the other */
	CHECK(pin(or, 1764018u, 0x92567396u, &st) == -SWW_E_IMG && st);
	CHECK(pin(or, 1764091u, 0x92567397u, &st) == -SWW_E_IMG && st);
	CHECK(pin(or, 1764090u, 0x92567396u, &st) == -SWW_E_IMG && st);
	CHECK(pin(0, 0u, 0u, &st) == -SWW_E_IMG && st);                         /* no identity set (pin 0) = stop, even for an empty image */
	CHECK(pin(0, 1764091u, 0x92567396u, &st) == -SWW_E_IMG && st);
	CHECK(nbus == 0);

	if (fails) {
		fprintf(stderr, "RESULT: FAIL wlan_fwid_host_test %u checks failed\n", fails);
		return 1;
	}
	printf("RESULT: PASS wlan_fwid_host_test\n");
	return 0;
}
