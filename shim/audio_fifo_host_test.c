/* Host test of the /dev/sweetaudio FIFO (xnu patch 0173, bsd/dev/sweet_audio_fifo.h, sweetaoc=8): push bound + overrun count, the 960-frame
 * start threshold, underrun = silence + restart threshold, close play-out below the threshold, wrap, flush, gain clamp (<= -10 dB, default
 * -20 dB, -96 dB = mute) and the per-channel scale. */
#include <stdio.h>
#include <stdint.h>
#include "sweet_audio_fifo.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct swau_fifo f;
static uint32_t in[SWAU_FIFO_FRAMES + 100], out[SWAU_FIFO_FRAMES + 100];

int
main(void)
{
	uint32_t i, n;

	for (i = 0; i < SWAU_FIFO_FRAMES + 100; i++)
		in[i] = (uint32_t)(uint16_t)(int16_t)(i & 0x3FFF) | (uint32_t)(uint16_t)(int16_t)-(int32_t)(i & 0x3FFF) << 16;
	swau_fifo_reset(&f);
	CHECK(f.gain_mdb == -20000 && f.gain_q16 == 6553u, "default gain %d q %u", f.gain_mdb, f.gain_q16);
	/* below the start threshold nothing leaves */
	CHECK(swau_fifo_push(&f, in, 959) == 959, "push 959");
	CHECK(swau_fifo_pull(&f, out, 192) == 0, "no drain below 960");
	CHECK(swau_fifo_push(&f, in + 959, 1) == 1, "push 1");
	n = swau_fifo_pull(&f, out, 192);
	CHECK(n == 192 && f.draining, "drain from 960: %u", n);
	CHECK(out[1] == swau_scale_word(in[1], 6553u), "scaled");
	/* drain to empty: underrun counted once, then the threshold again */
	for (i = 0; i < 4; i++)
		(void)swau_fifo_pull(&f, out, 192);
	CHECK(swau_fifo_fill(&f) == 0 && f.draining, "exactly empty, still draining");
	CHECK(swau_fifo_pull(&f, out, 192) == 0 && f.underruns == 1 && !f.draining, "underrun %u", f.underruns);
	CHECK(swau_fifo_push(&f, in, 500) == 500 && swau_fifo_pull(&f, out, 192) == 0, "threshold again after underrun");
	/* close: plays out below the threshold, no underrun counted */
	f.closing = 1;
	CHECK(swau_fifo_pull(&f, out, 192) == 192, "close play-out");
	CHECK(swau_fifo_pull(&f, out, 400) == 308 && f.underruns == 1, "rest played, no underrun: %u", f.underruns);
	CHECK(f.played == 960 + 500, "played %llu", (unsigned long long)f.played);
	/* full FIFO: push bounded, overruns counted, wrap keeps order */
	swau_fifo_reset(&f);
	swau_fifo_set_gain(&f, 0);
	CHECK(f.gain_mdb == -10000 && f.gain_q16 == 20724u, "clamp to -10 dB: %d %u", f.gain_mdb, f.gain_q16);
	CHECK(swau_fifo_push(&f, in, SWAU_FIFO_FRAMES + 100) == SWAU_FIFO_FRAMES && f.overruns == 100, "bound %u", f.overruns);
	CHECK(swau_fifo_push(&f, in, 4) == 0, "full: 0");
	CHECK(swau_fifo_pull(&f, out, 1000) == 1000, "pull 1000");
	CHECK(swau_fifo_push(&f, in + 7000, 1000) == 1000, "wrap push");
	CHECK(swau_fifo_pull(&f, out, SWAU_FIFO_FRAMES) == SWAU_FIFO_FRAMES, "pull all");
	CHECK(out[SWAU_FIFO_FRAMES - 1000] == swau_scale_word(in[7000], 20724u) && out[0] == swau_scale_word(in[1000], 20724u), "order across the wrap");
	/* flush */
	CHECK(swau_fifo_push(&f, in, 2000) == 2000, "push");
	swau_fifo_flush(&f);
	CHECK(swau_fifo_fill(&f) == 0 && swau_fifo_pull(&f, out, 10) == 0, "flushed");
	/* gain table */
	CHECK(swau_gain_q16(-96000) == 0u && swau_gain_q16(-200000) == 0u, "mute");
	CHECK(swau_gain_q16(-40000) == 655u && swau_gain_q16(-26000) == 3284u, "-40 %u -26 %u", swau_gain_q16(-40000), swau_gain_q16(-26000));
	CHECK(swau_gain_q16(5000) == 20724u, "positive clamps");
	/* scale: full-scale negative at -10 dB stays in range, channels independent */
	CHECK(swau_scale_word(0x7FFF8000u, 20724u) == ((uint32_t)(uint16_t)(int16_t)10361 << 16 | (uint16_t)(int16_t)-10362), "scale %#x",
	    swau_scale_word(0x7FFF8000u, 20724u));
	if (fails) {
		printf("audio_fifo_host_test: %d FAIL\n", fails);
		return 1;
	}
	printf("audio_fifo_host_test: PASS\n");
	return 0;
}
