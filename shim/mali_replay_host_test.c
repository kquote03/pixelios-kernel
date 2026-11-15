/* Host unit test for the pure part of the Mali job replay (xnu patch 0155, bsd/dev/sweet_mali_replay.h): the GPUJOBS1 container parser,
 * the .malijob v1 parser (docs/research/52 s.6), the fixed-VA reservation in the GPU VA allocator, crc32 and the render-target check.
 * shim/build.sh extracts both headers from the patches (sweet_mali_pt.h from 0104, sweet_mali_replay.h from 0155), so the kernel and this
 * test see the same bytes. Built twice: -Werror, and with ASan/UBSan (every length and offset in the inputs is attacker-shaped here).
 *   gcc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iout -o out/mali_replay_asan_host_test mali_replay_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sweet_mali_pt.h"
#include "sweet_mali_replay.h"

#define GPU 0x92020010u

static void
w32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static void
w64(uint8_t *p, uint64_t v)
{
	w32(p, (uint32_t)v);
	w32(p + 4, (uint32_t)(v >> 32));
}

/* a job like tri.malijob: 3 BOs (heap without data, a descriptor BO holding both job chains, the RT), RT 64x32 */
#define NB 3
static size_t
make_job(uint8_t *f, size_t cap)
{
	memset(f, 0, cap);
	const uint32_t chain_off = 64, rt_off = 192, bo_off = 256, data_off = bo_off + 64 * NB;
	memcpy(f, "PIXMJOB1", 8);
	w32(f + 8, 1);
	w32(f + 12, 64);
	w32(f + 16, GPU);
	w32(f + 20, 2);
	w32(f + 24, NB);
	w32(f + 28, 1);                         /* RT block present */
	w32(f + 40, chain_off);
	w32(f + 44, rt_off);
	w32(f + 48, bo_off);
	w32(f + 52, data_off);
	memcpy(f + 56, "tri", 3);
	w64(f + chain_off, 0x03054300ull);      /* vertex/tiler */
	w32(f + chain_off + 8, 0);
	w64(f + chain_off + 16, 0x03054680ull); /* fragment */
	w32(f + chain_off + 24, 1);
	uint8_t *rt = f + rt_off;
	w64(rt, 0x03100000ull);
	w32(rt + 8, 64);
	w32(rt + 12, 32);
	w32(rt + 16, 256);
	w32(rt + 20, 1);
	w32(rt + 24, 0);
	w32(rt + 28, 0);
	const uint16_t px[3][2] = { { 0, 0 }, { 63, 31 }, { 32, 16 } };
	const uint32_t col[3] = { 0xff996633u, 0xff996633u, 0xff00ff00u };
	for (int i = 0; i < 3; i++) {
		rt[32 + 8 * i] = (uint8_t)px[i][0];
		rt[33 + 8 * i] = (uint8_t)(px[i][0] >> 8);
		rt[34 + 8 * i] = (uint8_t)px[i][1];
		rt[35 + 8 * i] = (uint8_t)(px[i][1] >> 8);
		w32(rt + 36 + 8 * i, col[i]);
	}
	const uint64_t va[NB] = { 0x02000000ull, 0x03054000ull, 0x03100000ull }, sz[NB] = { 0x1000000ull, 0x10000ull, 0x4000ull };
	const uint64_t dl[NB] = { 0, 0x700, 0 };
	uint64_t off = data_off;
	for (int i = 0; i < NB; i++) {
		uint8_t *b = f + bo_off + 64 * i;
		w64(b, va[i]);
		w64(b + 8, sz[i]);
		w64(b + 16, off);
		w64(b + 24, dl[i]);
		w32(b + 32, i == 0 ? 6u : 0u);
		snprintf((char *)b + 40, 24, "bo%d", i);
		for (uint64_t k = 0; k < dl[i]; k++) {
			f[off + k] = (uint8_t)(k * 7 + 1);
		}
		off += dl[i];
	}
	assert(off <= cap);
	w64(f + 32, off);
	return (size_t)off;
}

/* parse an exact-size heap copy, so ASan sees any read past the length the parser was given */
static int
parse(const uint8_t *f, size_t n)
{
	struct smr_job j;
	uint8_t *h = malloc(n ? n : 1);
	assert(h);
	memcpy(h, f, n);
	int r = smr_job_parse(h, n, GPU, &j);
	free(h);
	return r;
}

static int
cparse(const uint8_t *c, size_t n)
{
	struct smr_entry e[SMR_C_MAX_ENTRIES];
	unsigned cnt = 0;
	uint8_t *h = malloc(n ? n : 1);
	assert(h);
	memcpy(h, c, n);
	int r = smr_container_parse(h, n, e, &cnt);
	free(h);
	return r;
}

/* generic valid job: nch chains (last one fragment, all inside BO 0), nbo BOs; BO 0 = 64 KiB of descriptors at 0x03000000, the last BO holds the
 * RT; any other BO is 16 KiB, packed after BO 0. Compact layout: chain_off 64, rt_off, bo_off, data_off follow each other. */
struct bospec {
	uint64_t va, size, dlen;
};

static size_t
build(uint8_t *f, size_t cap, unsigned nch, unsigned nbo, const struct bospec *bo, uint32_t w, uint32_t h, uint32_t stride)
{
	memset(f, 0, cap);
	const uint32_t chain_off = 64, rt_off = chain_off + 16 * nch, bo_off = rt_off + 64, data_off = bo_off + 64 * nbo;
	memcpy(f, "PIXMJOB1", 8);
	w32(f + 8, 1);
	w32(f + 12, 64);
	w32(f + 16, GPU);
	w32(f + 20, nch);
	w32(f + 24, nbo);
	w32(f + 28, 1);
	w32(f + 40, chain_off);
	w32(f + 44, rt_off);
	w32(f + 48, bo_off);
	w32(f + 52, data_off);
	memcpy(f + 56, "gen", 3);
	for (unsigned i = 0; i < nch; i++) {
		w64(f + chain_off + 16 * i, bo[0].va + 128ull * i);
		w32(f + chain_off + 16 * i + 8, i + 1 == nch ? 1u : 0u);
	}
	w64(f + rt_off, bo[nbo - 1].va);
	w32(f + rt_off + 8, w);
	w32(f + rt_off + 12, h);
	w32(f + rt_off + 16, stride);
	w32(f + rt_off + 20, 1);
	const uint32_t px[3][2] = { { 0, 0 }, { w - 1, h - 1 }, { w / 2, h / 2 } };
	for (int i = 0; i < 3; i++) {
		f[rt_off + 32 + 8 * i] = (uint8_t)px[i][0];
		f[rt_off + 33 + 8 * i] = (uint8_t)(px[i][0] >> 8);
		f[rt_off + 34 + 8 * i] = (uint8_t)px[i][1];
		f[rt_off + 35 + 8 * i] = (uint8_t)(px[i][1] >> 8);
	}
	uint64_t off = data_off;
	for (unsigned i = 0; i < nbo; i++) {
		uint8_t *b = f + bo_off + 64 * i;
		w64(b, bo[i].va);
		w64(b + 8, bo[i].size);
		w64(b + 16, off);
		w64(b + 24, bo[i].dlen);
		off += bo[i].dlen;
	}
	assert(off <= cap);
	for (uint64_t k = data_off; k < off; k++) {
		f[k] = (uint8_t)(k | 1);
	}
	w64(f + 32, off);
	return (size_t)off;
}

/* set a field, expect a refusal, restore */
static void
expect_refused32(uint8_t *f, size_t n, size_t at, uint32_t v)
{
	uint8_t save[4];
	memcpy(save, f + at, 4);
	w32(f + at, v);
	int r = parse(f, n);
	if (r == 0) {
		fprintf(stderr, "FAIL: u32 %#x at %zu accepted\n", v, at);
		exit(1);
	}
	memcpy(f + at, save, 4);
}

static void
expect_refused64(uint8_t *f, size_t n, size_t at, uint64_t v)
{
	uint8_t save[8];
	memcpy(save, f + at, 8);
	w64(f + at, v);
	int r = parse(f, n);
	if (r == 0) {
		fprintf(stderr, "FAIL: u64 %#llx at %zu accepted\n", (unsigned long long)v, at);
		exit(1);
	}
	memcpy(f + at, save, 8);
}

static size_t
make_container_a(uint8_t *c, size_t cap, const uint8_t *const *blob, const size_t *len, unsigned n, size_t align)
{
	memset(c, 0, cap);
	memcpy(c, "GPUJOBS1", 8);
	w32(c + 8, 1);
	w32(c + 12, n);
	size_t off = 16 + 16 * (size_t)n;
	for (unsigned i = 0; i < n; i++) {
		off = (off + align - 1) & ~(align - 1);
		if (align == 4 && (off & 7) == 0) {
			off += 4;                      /* force a 4- but not 8-aligned entry */
		}
		memcpy(c + 16 + 16 * i, "GPUJ", 4);
		w32(c + 16 + 16 * i + 4, (uint32_t)len[i]);
		w32(c + 16 + 16 * i + 8, (uint32_t)off);
		w32(c + 16 + 16 * i + 12, smr_crc32(0, blob[i], len[i]));
		memcpy(c + off, blob[i], len[i]);
		off += len[i];
	}
	assert(off <= cap);
	return (off + 7) & ~(size_t)7;
}

static size_t
make_container(uint8_t *c, size_t cap, const uint8_t *const *blob, const size_t *len, unsigned n)
{
	return make_container_a(c, cap, blob, len, n, 8);
}

int
main(void)
{
	static uint8_t f[1 << 14], g[1 << 14], c[1 << 16];
	struct smr_job j;

	/* crc32 = zlib crc32 (known answer), and the expected crc of clear.malijob (docs/research/52 s.5, cross-checked with Python zlib) */
	assert(smr_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
	{
		static uint8_t rgba[256 * 256 * 4];
		for (size_t i = 0; i < sizeof(rgba); i += 4) {
			rgba[i] = 0x33;
			rgba[i + 1] = 0x66;
			rgba[i + 2] = 0x99;
			rgba[i + 3] = 0xff;
		}
		assert(smr_crc32(0, rgba, sizeof(rgba)) == 0xff12cb5cu);
	}

	/* ---- a valid job ---- */
	size_t n = make_job(f, sizeof(f));
	assert(smr_job_parse(f, n, GPU, &j) == 0);
	assert(j.n_chains == 2 && j.n_bos == NB && !strcmp(j.name, "tri"));
	assert(j.jc[0] == 0x03054300ull && j.req[0] == 0 && j.jc[1] == 0x03054680ull && j.req[1] == 1);
	assert(j.rt.va == 0x03100000ull && j.rt.width == 64 && j.rt.height == 32 && j.rt.stride == 256 && j.rt_bo == 2 && j.rt_off == 0);
	assert(j.bo_bytes == 0x1000000ull + 0x10000ull + 0x4000ull);
	struct smr_bo b;
	smr_job_bo(&j, 1, &b);
	assert(b.va == 0x03054000ull && b.size == 0x10000ull && b.data_len == 0x700 && b.flags == 0 && !strcmp(b.label, "bo1"));
	smr_job_bo(&j, 0, &b);
	assert(b.data_len == 0 && b.flags == 6);
	assert(smr_job_parse(f, n, 0x72120000u, &j) == SMR_E_GPU);        /* a capture for another GPU */

	/* ---- every truncation and every extension is refused (file_size must equal the length) ---- */
	for (size_t k = 0; k < n; k++) {
		assert(parse(f, k) != 0);
	}
	assert(parse(f, n + 1) != 0);

	/* ---- header fields ---- */
	expect_refused32(f, n, 0, 0x584d5850u);          /* magic */
	expect_refused32(f, n, 8, 2);                    /* version */
	expect_refused32(f, n, 12, 63);                  /* header_size */
	expect_refused32(f, n, 20, 0);                   /* n_chains */
	expect_refused32(f, n, 20, SMR_MAX_CHAINS + 1);
	expect_refused32(f, n, 24, 0);                   /* n_bos */
	expect_refused32(f, n, 24, SMR_MAX_BOS + 1);
	expect_refused32(f, n, 24, 0xFFFFFFFFu);
	expect_refused32(f, n, 28, 0);                   /* no RT block */
	expect_refused64(f, n, 32, n - 1);               /* file_size */
	expect_refused32(f, n, 40, 0);                   /* chain table over the header */
	expect_refused32(f, n, 40, (uint32_t)n - 16);    /* chain table past the end */
	expect_refused32(f, n, 40, 0xFFFFFFF0u);
	expect_refused32(f, n, 44, (uint32_t)n - 32);    /* rt past the end */
	expect_refused32(f, n, 44, 0xFFFFFFC0u);
	expect_refused32(f, n, 48, (uint32_t)n - 64);    /* bo table past the end */
	expect_refused32(f, n, 48, 0xFFFFFFC0u);
	expect_refused32(f, n, 52, (uint32_t)n + 1);     /* data area past the end */
	expect_refused32(f, n, 52, 0);                   /* data area inside the header */
	f[56] = 'T';                                    /* name: [a-z0-9_] only */
	assert(parse(f, n) != 0);
	f[56] = 't';

	/* ---- chains ---- */
	expect_refused64(f, n, 64, 0x01000000ull);       /* jc below the window */
	expect_refused64(f, n, 64, 0x03054304ull);       /* unaligned */
	expect_refused64(f, n, 64, 0x03010000ull);       /* 128-aligned but not in a BO */
	expect_refused64(f, n, 64, 0x03064000ull);       /* exactly at the BO end */
	expect_refused64(f, n, 64, 0x03054340ull);       /* 64- but not 128-aligned (v9 Job Header align 128) */
	expect_refused64(f, n, 64, 0xFFFFFFFFFFFFFFC0ull);
	expect_refused32(f, n, 72, 2);                   /* unknown requirement bit */
	expect_refused32(f, n, 88, 0);                   /* no fragment chain left */

	/* ---- render target ---- */
	expect_refused64(f, n, 192, 0x03104000ull);      /* not in a BO */
	expect_refused64(f, n, 192, 0x03103F00ull);      /* starts in the BO, ends after it */
	expect_refused32(f, n, 192 + 8, 0);              /* width */
	expect_refused32(f, n, 192 + 8, SMR_MAX_RT_DIM + 1);
	expect_refused32(f, n, 192 + 12, 0);             /* height */
	expect_refused32(f, n, 192 + 12, 0x10000000u);   /* stride*height overflow attempt */
	expect_refused32(f, n, 192 + 16, 255);           /* stride < width*4 */
	expect_refused32(f, n, 192 + 16, 0x80000000u);
	expect_refused32(f, n, 192 + 20, 2);             /* format */
	expect_refused32(f, n, 192 + 28, 2);             /* crc_valid */
	expect_refused32(f, n, 192 + 32, 64);            /* pixel 0 x = width */
	expect_refused32(f, n, 192 + 32, 32u << 16);     /* pixel 0 y = height */

	/* ---- BO entries ---- */
	for (int i = 0; i < NB; i++) {
		size_t e = 256 + 64 * (size_t)i;
		expect_refused64(f, n, e, 0x01FFC000ull);              /* below the window */
		expect_refused64(f, n, e, 0x03055000ull);              /* not 16 KiB aligned */
		expect_refused64(f, n, e, 0xFFFFC000ull);              /* end past 4 GiB */
		expect_refused64(f, n, e, 0xFFFFFFFFFFFFC000ull);      /* overflow */
		expect_refused64(f, n, e + 8, 0);                      /* size */
		expect_refused64(f, n, e + 8, 0x5000ull);
		expect_refused64(f, n, e + 8, SMR_MAX_BO_BYTES + 0x4000ull);
		expect_refused64(f, n, e + 8, 0xFFFFFFFFFFFFC000ull);
		expect_refused64(f, n, e + 16, n + 1);                 /* data_off past the end */
		expect_refused64(f, n, e + 16, 0xFFFFFFFFFFFFFFF0ull);
		expect_refused64(f, n, e + 24, 0x1000001ull);          /* data_len > size */
		expect_refused64(f, n, e + 24, 0xFFFFFFFFFFFFFFF0ull);
		expect_refused32(f, n, e + 32, 8);                     /* unknown flag */
		f[e + 63] = 'x';                                      /* label must be NUL-terminated */
		assert(parse(f, n) != 0);
		f[e + 63] = 0;
	}
	expect_refused64(f, n, 256 + 24, n);                        /* BO 0 data would reach past the file */
	expect_refused64(f, n, 256 + 64 + 16, 64);                  /* BO data inside the tables */
	expect_refused64(f, n, 256 + 64, 0x02FFC000ull);            /* BO 1 overlaps the heap */
	expect_refused64(f, n, 256 + 128, 0x03054000ull);           /* BO 2 = BO 1 */
	assert(parse(f, n) == 0);                                   /* every restore was exact */

	/* ---- random corruption never crashes (ASan/UBSan build) and a changed byte inside the tables is either refused or harmless ---- */
	{
		uint32_t s = 12345;
		for (int it = 0; it < 200000; it++) {
			memcpy(g, f, n);
			int flips = 1 + (int)(s % 4);
			for (int k = 0; k < flips; k++) {
				s = s * 1103515245u + 12345u;
				g[(s >> 8) % 448] ^= (uint8_t)(1u << ((s >> 4) & 7));
			}
			s = s * 1103515245u + 12345u;
			if (smr_job_parse(g, n, GPU, &j) == 0) {
				/* accepted: the invariants the kernel relies on must still hold */
				for (unsigned i = 0; i < j.n_bos; i++) {
					smr_job_bo(&j, i, &b);
					assert(b.va >= MALI_VA_START && b.size <= MALI_VA_END - b.va && (b.va & 0x3FFF) == 0 && (b.size & 0x3FFF) == 0);
					assert(b.data_len <= b.size && b.data_off <= n && b.data_len <= n - b.data_off);
				}
				assert(j.rt_bo < j.n_bos);
			}
		}
	}

	/* ---- render-target check ---- */
	{
		static uint8_t rt[256 * 32];
		assert(smr_job_parse(f, n, GPU, &j) == 0);
		for (unsigned y = 0; y < 32; y++) {
			for (unsigned x = 0; x < 64; x++) {
				uint32_t v = (x == 32 && y == 16) ? 0xff00ff00u : 0xff996633u;
				w32(rt + y * 256 + x * 4, v);
			}
		}
		uint32_t crc = 0, want = 0;
		for (unsigned y = 0; y < 32; y++) {
			want = smr_crc32(want, rt + y * 256, 64 * 4);
		}
		assert(smr_rt_check(&j, rt, &crc) == 0 && crc == want);
		w32(rt + 16 * 256 + 32 * 4, 0xff996633u);              /* triangle pixel missing */
		assert(smr_rt_check(&j, rt, &crc) == 1);
		/* crc_valid: the crc must match too */
		w32(f + 192 + 24, want);
		w32(f + 192 + 28, 1);
		assert(smr_job_parse(f, n, GPU, &j) == 0);
		assert(smr_rt_check(&j, rt, &crc) == 2);               /* pixel + crc */
		w32(rt + 16 * 256 + 32 * 4, 0xff00ff00u);
		assert(smr_rt_check(&j, rt, &crc) == 0);
		w32(f + 192 + 28, 0);
	}

	/* ---- GPUJOBS1 container ---- */
	{
		n = make_job(f, sizeof(f));
		memcpy(g, f, n);
		const uint8_t *blobs[2] = { f, g };
		const size_t lens[2] = { n, n };
		size_t cn = make_container(c, sizeof(c), blobs, lens, 2);
		struct smr_entry e[SMR_C_MAX_ENTRIES];
		unsigned cnt = 0;
		assert(smr_container_parse(c, cn, e, &cnt) == 0 && cnt == 2 && e[0].length == n && e[1].offset % 8 == 0);
		assert(smr_job_parse(c + e[1].offset, e[1].length, GPU, &j) == 0);
		for (size_t k = 0; k < 16 + 32; k++) {
			assert(cparse(c, k) != 0);
		}
		uint8_t save = c[e[1].offset + 100];
		c[e[1].offset + 100] ^= 1;                              /* crc */
		assert(cparse(c, cn) != 0);
		c[e[1].offset + 100] = save;
		static const size_t fld[] = { 0, 8, 12, 16, 20, 24, 32, 36, 40 };
		static const uint32_t val[] = { 0x58585858u, 2, 0, 0x4a555048u, 0xFFFFFF00u, 12, 0, 0, 0x7FFFFFF8u };
		for (size_t k = 0; k < sizeof(fld) / sizeof(fld[0]); k++) {
			uint8_t s4[4];
			memcpy(s4, c + fld[k], 4);
			w32(c + fld[k], val[k]);
			assert(cparse(c, cn) != 0);
			memcpy(c + fld[k], s4, 4);
		}
		w32(c + 12, SMR_C_MAX_ENTRIES + 1);
		assert(cparse(c, cn) != 0);
		w32(c + 12, 2);
		assert(smr_container_parse(c, SMR_C_MAX_BYTES + 1, e, &cnt) != 0);
		assert(smr_container_parse(c, cn, e, &cnt) == 0);
		uint32_t s = 777;
		for (int it = 0; it < 50000; it++) {
			static uint8_t d[1 << 16];
			memcpy(d, c, cn);
			s = s * 1103515245u + 12345u;
			d[(s >> 8) % 48] ^= (uint8_t)(1u << ((s >> 4) & 7));
			if (smr_container_parse(d, cn, e, &cnt) == 0) {
				for (unsigned i = 0; i < cnt; i++) {
					assert(e[i].offset <= cn && e[i].length <= cn - e[i].offset);
				}
			}
		}
	}

	/* ---- cases where ONLY one rule is broken (each kills its mutant in tools/test_mali_replay_mutants.py) ---- */
	{
		static uint8_t h[1 << 20];
		static struct bospec bo[SMR_MAX_BOS + 1];
		const struct bospec base[2] = { { 0x03000000ull, 0x10000ull, 0x400 }, { 0x03100000ull, 0x8000ull, 0 } };
		size_t hn;

		hn = build(h, sizeof(h), 2, 2, base, 64, 32, 256);                  /* the generic builder itself is valid */
		assert(parse(h, hn) == 0);
		hn = build(h, sizeof(h), SMR_MAX_CHAINS, 2, base, 64, 32, 256);
		assert(parse(h, hn) == 0);
		hn = build(h, sizeof(h), SMR_MAX_CHAINS + 1, 2, base, 64, 32, 256);  /* 9 valid chains */
		assert(parse(h, hn) == SMR_E_HDR);

		bo[0] = base[0];
		for (unsigned i = 1; i <= SMR_MAX_BOS; i++) {
			bo[i] = (struct bospec){ 0x03100000ull + 0x4000ull * i, 0x4000ull, 16 };
		}
		hn = build(h, sizeof(h), 2, SMR_MAX_BOS, bo, 64, 32, 256);
		assert(parse(h, hn) == 0);
		hn = build(h, sizeof(h), 2, SMR_MAX_BOS + 1, bo, 64, 32, 256);      /* 65 valid BOs */
		assert(parse(h, hn) == SMR_E_HDR);

		/* data_len larger than the BO, data still inside the file */
		struct bospec big[2] = { { 0x03000000ull, 0x10000ull, 0x4010 }, { 0x03100000ull, 0x8000ull, 0 } };
		hn = build(h, sizeof(h), 2, 2, big, 64, 32, 256);
		assert(parse(h, hn) == 0);
		w64(h + 64 + 16 * 2 + 64 + 8, 0x4000ull);                          /* BO 0 size 16 KiB < data_len */
		assert(parse(h, hn) == SMR_E_BO);

		/* a third BO that overlaps BO 0 but holds neither a chain nor the RT */
		struct bospec ov[3] = { { 0x03000000ull, 0x10000ull, 0x400 }, { 0x03008000ull, 0x4000ull, 0 }, { 0x03100000ull, 0x8000ull, 0 } };
		hn = build(h, sizeof(h), 2, 3, ov, 64, 32, 256);
		assert(parse(h, hn) == SMR_E_OVERLAP);
		ov[1].va = 0x03010000ull;
		hn = build(h, sizeof(h), 2, 3, ov, 64, 32, 256);
		assert(parse(h, hn) == 0);

		/* RT wider than SMR_MAX_RT_DIM that still fits its BO */
		hn = build(h, sizeof(h), 2, 2, base, SMR_MAX_RT_DIM, 1, SMR_MAX_RT_DIM * 4);
		assert(parse(h, hn) == 0);
		hn = build(h, sizeof(h), 2, 2, base, SMR_MAX_RT_DIM + 1, 1, (SMR_MAX_RT_DIM + 1) * 4);
		assert(parse(h, hn) == SMR_E_RT);

		/* container: 5 valid entries, a tag that only differs in the last byte, 4-aligned entries, an entry inside the header */
		hn = build(h, sizeof(h), 2, 2, base, 64, 32, 256);
		const uint8_t *bl[5] = { h, h, h, h, h };
		const size_t ln[5] = { hn, hn, hn, hn, hn };
		static uint8_t cc[1 << 16];
		size_t ccn = make_container(cc, sizeof(cc), bl, ln, SMR_C_MAX_ENTRIES);
		assert(cparse(cc, ccn) == 0);
		ccn = make_container(cc, sizeof(cc), bl, ln, SMR_C_MAX_ENTRIES + 1);
		assert(cparse(cc, ccn) == 3);
		ccn = make_container(cc, sizeof(cc), bl, ln, 1);
		cc[16 + 3] = 'X';                                                     /* "GPUX" */
		assert(cparse(cc, ccn) == 4);
		ccn = make_container_a(cc, sizeof(cc), bl, ln, 1, 4);
		assert(cparse(cc, ccn) == 5);
		ccn = make_container(cc, sizeof(cc), bl, ln, 1);
		w32(cc + 16 + 4, 16);                                                 /* entry = the container header itself, crc made valid */
		w32(cc + 16 + 8, 0);
		w32(cc + 16 + 12, smr_crc32(0, cc, 16));
		assert(cparse(cc, ccn) == 5);
	}

	/* ---- fixed-VA reservation (the replay maps BOs at their recorded VAs) ---- */
	{
		static struct mali_va v;
		assert(mali_va_reserve(&v, 0x03000000ull, 4) == 0);
		assert(mali_va_reserve(&v, 0x03000000ull, 4) == -2);              /* taken */
		assert(mali_va_reserve(&v, 0x02FFC000ull, 8) == -2);              /* overlaps the end of the free part: refused, nothing set */
		assert(!mali_va_test(&v, (uint32_t)((0x02FFC000ull - MALI_VA_START) / MALI_PG)));
		assert(mali_va_reserve(&v, 0x01FFC000ull, 4) == -1);              /* below the window */
		assert(mali_va_reserve(&v, MALI_VA_END - 0x4000ull, 4) == 0);     /* last 16 KiB */
		assert(mali_va_reserve(&v, MALI_VA_END, 4) == -1);
		assert(mali_va_reserve(&v, MALI_VA_END - 0x4000ull, 8) == -1);    /* runs past the end */
		assert(mali_va_reserve(&v, 0x03001000ull, 4) == -1);              /* not 16 KiB aligned */
		assert(mali_va_reserve(&v, 0x04000000ull, 0) == -1);
		assert(mali_va_reserve(&v, 0x04000000ull, 6) == -1);              /* not a multiple of 4 pages */
		assert(mali_va_reserve(&v, 0xFFFFFFFFFFFFC000ull, 4) == -1);
		/* the first-fit allocator does not hand out a reserved range, and free() releases it */
		uint64_t a = mali_va_alloc(&v, 0x1000000u / 0x1000u, 4);         /* 16 MiB at the start: ends below 0x03000000 */
		assert(a == MALI_VA_START);
		uint64_t b2 = mali_va_alloc(&v, 4, 4);
		assert(b2 == 0x03004000ull);
		assert(mali_va_free(&v, 0x03000000ull, 4) == 0 && mali_va_reserve(&v, 0x03000000ull, 4) == 0);
	}

	printf("mali_replay_host_test: PASS\n");
	return 0;
}
