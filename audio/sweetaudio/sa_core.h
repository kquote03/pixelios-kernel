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

/* ---- libsalog tap (salog.c, doc 55): make-up gain + block peak limiter, mixing of concurrent taps, drift trim ---- */

/* Block limiter: gain g starts at the make-up gain gmax; a block whose peak * g would exceed ceil gets g = ceil / peak at once (attack in
 * the same block, so the output never exceeds ceil); otherwise g recovers by the factor rel per block, up to gmax. Returns the gain. */
struct sa_lim { float g, gmax, ceil, rel; };

static inline void
sa_lim_init(struct sa_lim *l, float gmax, float ceil, float rel)
{
	l->g = l->gmax = gmax;
	l->ceil = ceil;
	l->rel = rel;
}

static inline float
sa_lim_block(struct sa_lim *l, float peak)
{
	float g = l->g * l->rel;
	if (g > l->gmax)
		g = l->gmax;
	if (peak * g > l->ceil)
		g = l->ceil / peak;
	l->g = g;
	return g;
}

/* Mix accumulator for several writers (VirtualAudio IOProcs of different aggregates in one process): writer w adds its frames at its own
 * offset *off (frames ahead of the accumulator start), summing over frames other writers already put there; sa_mix_take removes the first
 * n frames (flushed to the kernel) and moves every writer offset back by n (a writer that fell behind restarts at 0). */
#define SA_MIXN 8192u
struct sa_mix { float acc[SA_MIXN * 2]; uint32_t len; };

static inline uint32_t
sa_mix_add(struct sa_mix *m, uint32_t *off, const float *x, uint32_t k)
{
	if (*off > m->len)
		*off = m->len;
	if (k > SA_MIXN - *off)
		k = SA_MIXN - *off;
	for (uint32_t i = 0; i < k; i++) {
		uint32_t f = *off + i;
		for (uint32_t c = 0; c < 2; c++)
			m->acc[2 * f + c] = (f < m->len ? m->acc[2 * f + c] : 0.0f) + x[2 * i + c];
	}
	*off += k;
	if (*off > m->len)
		m->len = *off;
	return k;
}

static inline void
sa_mix_take(struct sa_mix *m, uint32_t n, uint32_t *const *offs, uint32_t noffs)
{
	if (n > m->len)
		n = m->len;
	for (uint32_t i = 0; i < 2 * (m->len - n); i++)
		m->acc[i] = m->acc[i + 2 * n];
	m->len -= n;
	for (uint32_t w = 0; w < noffs; w++)
		*offs[w] = *offs[w] > n ? *offs[w] - n : 0;
}

/* Drift trim from the kernel FIFO fill (SWAU_IOC_POS before each write): a slow average of the fill vs a target gives a resample ratio
 * correction (+ = take more input per output frame), bounded to +-lim; an empty FIFO (start, underrun) resets the average. */
static inline double
sa_drift_adj(double *avg, uint32_t fill, double target, double k, double lim)
{
	if (fill == 0) {
		*avg = target;
		return 0.0;
	}
	*avg += 0.02 * ((double)fill - *avg);
	double a = (*avg - target) * k;
	return a > lim ? lim : a < -lim ? -lim : a;
}

#endif
