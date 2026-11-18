/* SPDX-License-Identifier: MIT */
/* Pure logic of SweetAudio.driver (no CoreAudio, no syscalls): host-tested by sa_host_test.c. */
#ifndef SA_CORE_H
#define SA_CORE_H

#include <stdint.h>

/* Float32 mix (nominal -1..1, may exceed) to S16LE with clipping and round-to-nearest. NaN -> 0. */
static inline int16_t
sa_f2s16(float x)
{
	if (!(x == x))
		return 0;
	if (x >= 1.0f)
		return 32767;
	if (x <= -1.0f)
		return -32768;
	float y = x * 32768.0f;
	int32_t v = (int32_t)(y >= 0.0f ? y + 0.5f : y - 0.5f);
	if (v > 32767)
		v = 32767;
	if (v < -32768)
		v = -32768;
	return (int16_t)v;
}

static inline void
sa_convert(const float *in, int16_t *out, uint32_t samples)
{
	for (uint32_t i = 0; i < samples; i++)
		out[i] = sa_f2s16(in[i]);
}

/*
 * Zero time stamps of a free-running device clock derived from the host clock (the AudioServerPlugIn pattern).
 * anchor = host time at StartIO, ticks_per_frame = host ticks per frame (double), period = ZeroTimeStampPeriod frames.
 * Returns the newest zero time stamp at or before now: sample = n * period, host = anchor + n * period * ticks_per_frame.
 * n only moves forward (*count is the state). Bounded: at most one step per call is the normal case; a late caller
 * (now far ahead) jumps directly to the right n.
 */
struct sa_clock {
	uint64_t anchor;
	double   ticks_per_frame;
	uint32_t period;
	uint64_t count;
};

static inline void
sa_zero_ts(struct sa_clock *c, uint64_t now, double *sample, uint64_t *host)
{
	double ticks_per_period = c->ticks_per_frame * (double)c->period;
	if (now > c->anchor && ticks_per_period > 0.0) {
		uint64_t n = (uint64_t)((double)(now - c->anchor) / ticks_per_period);
		/* the stamp of period n is at anchor + (uint64_t)(n * ticks_per_period) (the same truncation as below): fix
		 * the double rounding of the division in both directions (at most a step or two each) */
		for (int i = 0; i < 4 && c->anchor + (uint64_t)((double)(n + 1) * ticks_per_period) <= now; i++)
			n++;
		for (int i = 0; i < 4 && n > 0 && c->anchor + (uint64_t)((double)n * ticks_per_period) > now; i++)
			n--;
		if (n > c->count)
			c->count = n;
	}
	*sample = (double)c->count * (double)c->period;
	*host = c->anchor + (uint64_t)((double)c->count * ticks_per_period);
}

/* How many whole frames of a write of len bytes the kernel took (it returns a byte count). */
static inline uint32_t
sa_frames_taken(long rc, uint32_t frames)
{
	if (rc <= 0)
		return 0;
	uint64_t f = (uint64_t)rc / 4u;
	return f > frames ? frames : (uint32_t)f;
}

#endif
