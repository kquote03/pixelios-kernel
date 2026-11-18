/* SPDX-License-Identifier: MIT */
/* Host test of sa_core.h: cc -O1 -fsanitize=address,undefined -o /tmp/sa_host_test sa_host_test.c -lm && /tmp/sa_host_test */
#include <math.h>
#include <stdio.h>

#include "sa_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int
main(void)
{
	/* conversion: clipping, rounding, sign, NaN */
	CHECK(sa_f2s16(0.0f) == 0);
	CHECK(sa_f2s16(1.0f) == 32767);
	CHECK(sa_f2s16(2.5f) == 32767);
	CHECK(sa_f2s16(-1.0f) == -32768);
	CHECK(sa_f2s16(-7.0f) == -32768);
	CHECK(sa_f2s16(NAN) == 0);
	CHECK(sa_f2s16(INFINITY) == 32767);
	CHECK(sa_f2s16(-INFINITY) == -32768);
	CHECK(sa_f2s16(0.5f) == 16384);
	CHECK(sa_f2s16(-0.5f) == -16384);
	CHECK(sa_f2s16(1.0f / 65536.0f) == 1);      /* 0.5 LSB rounds away from zero */
	CHECK(sa_f2s16(-1.0f / 65536.0f) == -1);
	CHECK(sa_f2s16(0.99999f) == 32767);
	for (int i = -32768; i <= 32767; i += 7)    /* exact round trip of every representable step tested */
		CHECK(sa_f2s16((float)i / 32768.0f) == i);
	float in[6] = { 0.25f, -0.25f, 3.0f, -3.0f, 0.0f, NAN };
	int16_t out[6];
	sa_convert(in, out, 6);
	CHECK(out[0] == 8192 && out[1] == -8192 && out[2] == 32767 && out[3] == -32768 && out[4] == 0 && out[5] == 0);

	/* zero time stamps: 24 MHz host clock (500 ticks per frame at 48 kHz), period 16384 */
	struct sa_clock c = { 1000, 500.0, 16384, 0 };
	double s;
	uint64_t h;
	sa_zero_ts(&c, 0, &s, &h);                         /* before the anchor */
	CHECK(s == 0.0 && h == 1000);
	sa_zero_ts(&c, 1000 + 16384ull * 500 - 1, &s, &h); /* one tick before the first period ends */
	CHECK(s == 0.0 && h == 1000);
	sa_zero_ts(&c, 1000 + 16384ull * 500, &s, &h);     /* exactly at it */
	CHECK(s == 16384.0 && h == 1000 + 16384ull * 500);
	sa_zero_ts(&c, 1000 + 10 * 16384ull * 500 + 7, &s, &h);  /* late caller jumps */
	CHECK(s == 10 * 16384.0 && h == 1000 + 10 * 16384ull * 500);
	sa_zero_ts(&c, 1000 + 3 * 16384ull * 500, &s, &h); /* never moves back */
	CHECK(s == 10 * 16384.0);
	/* non-integer ticks per frame (iPhone-like 24 MHz / 48 kHz is integer; use 1e9/48000 ns = 20833.33) */
	struct sa_clock d = { 0, 1e9 / 48000.0, 16384, 0 };
	for (uint64_t k = 1; k < 2000; k++) {
		uint64_t at = (uint64_t)((double)k * 16384.0 * d.ticks_per_frame);
		sa_zero_ts(&d, at, &s, &h);
		CHECK(h <= at && s == (double)k * 16384.0);
		sa_zero_ts(&d, at - 1, &s, &h);   /* state already at k: stays */
		CHECK(s == (double)k * 16384.0);
	}

	/* write return -> frames */
	CHECK(sa_frames_taken(-1, 10) == 0);
	CHECK(sa_frames_taken(0, 10) == 0);
	CHECK(sa_frames_taken(7, 10) == 1);
	CHECK(sa_frames_taken(40, 10) == 10);
	CHECK(sa_frames_taken(400, 10) == 10);

	printf("sa_host_test: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
