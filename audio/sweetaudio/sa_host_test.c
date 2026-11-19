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

	/* tap limiter: make-up 4x, ceiling 0.89, release 1.06 per block */
	{
		struct sa_lim l;
		sa_lim_init(&l, 4.0f, 0.89f, 1.06f);
		CHECK(sa_lim_block(&l, 0.1f) == 4.0f);                 /* quiet: full make-up */
		float g = sa_lim_block(&l, 0.708f);                      /* loud: attack in the same block */
		CHECK(g * 0.708f <= 0.8901f && g * 0.708f >= 0.889f);
		float g2 = sa_lim_block(&l, 0.0f);                       /* silence: slow release */
		CHECK(g2 > g && g2 < g * 1.07f);
		for (int i = 0; i < 100; i++)
			g2 = sa_lim_block(&l, 0.01f);
		CHECK(g2 == 4.0f);
		CHECK(sa_lim_block(&l, 5.0f) * 5.0f <= 0.8901f);       /* > full scale input */
	}
	/* mix: two writers sum where they overlap, take moves offsets back */
	{
		static struct sa_mix m;
		uint32_t a = 0, b = 0, *offs[2] = { &a, &b };
		float x[8] = { 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f }, y[4] = { 0.5f, -0.5f, 0.5f, -0.5f };
		CHECK(sa_mix_add(&m, &a, x, 4) == 4 && m.len == 4 && a == 4);
		CHECK(sa_mix_add(&m, &b, y, 2) == 2 && m.len == 4 && b == 2);
		CHECK(m.acc[0] == 0.75f && m.acc[1] == -0.25f && m.acc[4] == 0.25f);
		sa_mix_take(&m, 4, offs, 2);
		CHECK(m.len == 0 && a == 0 && b == 0);
		CHECK(sa_mix_add(&m, &b, y, 2) == 2 && sa_mix_add(&m, &a, x, 4) == 4 && m.len == 4 && m.acc[2] == 0.75f && m.acc[4] == 0.25f);
		sa_mix_take(&m, 3, offs, 2);
		CHECK(m.len == 1 && a == 1 && b == 0 && m.acc[0] == 0.25f);
		b = 9;                                                    /* an offset past the end is pulled back */
		CHECK(sa_mix_add(&m, &b, y, 2) == 2 && b == 3 && m.len == 3);
		uint32_t big = SA_MIXN - 1, c = big;
		m.len = big;
		CHECK(sa_mix_add(&m, &c, x, 4) == 1 && m.len == SA_MIXN);  /* bounded at SA_MIXN */
	}
	/* drift trim */
	{
		double avg = 0;
		CHECK(sa_drift_adj(&avg, 0, 1200, 2e-6, 0.003) == 0.0 && avg == 1200);
		double a = 0;
		for (int i = 0; i < 500; i++)
			a = sa_drift_adj(&avg, 2200, 1200, 2e-6, 0.003);
		CHECK(a > 0.0019 && a <= 0.002);
		for (int i = 0; i < 500; i++)
			a = sa_drift_adj(&avg, 8000, 1200, 2e-6, 0.003);
		CHECK(a == 0.003);
		for (int i = 0; i < 2000; i++)
			a = sa_drift_adj(&avg, 100, 1200, 2e-6, 0.003);
		CHECK(a < -0.0021 && a >= -0.003);
	}

	/* system volume state (Step 8) */
	{
		CHECK(sa_vol_from_state(0) == 1.0f);                    /* no publisher: unity */
		CHECK(sa_vol_from_state(sa_vol_to_state(1.0f)) == 1.0f);
		CHECK(sa_vol_from_state(sa_vol_to_state(0.0f)) == 0.0f);
		float h = sa_vol_from_state(sa_vol_to_state(0.5f));
		CHECK(h > 0.2499f && h < 0.2501f);
		CHECK(sa_vol_to_state(7.0f) == sa_vol_to_state(1.0f));  /* clamped */
		CHECK(sa_vol_to_state(-1.0f) == 1u && sa_vol_to_state(0.0f / 0.0f) == 1u);
		CHECK(sa_vol_from_state(5000000u) == 1.0f);            /* out of range state: unity, never > 1 */
	}

	/* volume key steps (Step 10): 3 dB per level through the v^2 curve, level 0 = mute, round trip, clamp */
	{
		CHECK(sa_vol_from_state(sa_vol_to_state(sa_vol_level_v[0])) == 0.0f);
		CHECK(sa_vol_from_state(sa_vol_to_state(sa_vol_level_v[SA_VOL_LEVELS])) == 1.0f);
		for (int n = 1; n <= SA_VOL_LEVELS; n++) {
			float g = sa_vol_from_state(sa_vol_to_state(sa_vol_level_v[n]));
			float db = 20.0f * log10f(g);
			CHECK(fabsf(db - (float)(-SA_VOL_STEP_DB * (SA_VOL_LEVELS - n))) < 0.01f);
			CHECK(sa_vol_level_of(sa_vol_level_v[n]) == n);
		}
		CHECK(sa_vol_level_of(0.0f) == 0 && sa_vol_level_of(1.0f) == SA_VOL_LEVELS && sa_vol_level_of(0.0f / 0.0f) == 0);
		CHECK(sa_vol_level_clamp(-1) == 0 && sa_vol_level_clamp(17) == SA_VOL_LEVELS && sa_vol_level_clamp(5) == 5);
	}

	printf("sa_host_test: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
