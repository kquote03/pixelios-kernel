/* Host test of the AOC firmware load (xnu patch 0164, bsd/dev/sweet_aoc_load.h, sweetaoc=2, plan step A6) against a fake SoC:
 * PMU, aoc_req, GSA mailbox and a fake carveout. Every case checks the exact list of register writes: good image, wrong sha,
 * wrong length, bad header (two kinds), body larger than the carveout, AOC domain off, corrupted read-back, GSA error code,
 * undocumented GSA reply, mailbox timeout. After a stop nothing more is written (only the documented restores). SHA-256 is
 * checked against FIPS 180-4 vectors and the pinned identity against docs/research/50. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sweet_aoc_load.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { GSA_OK, GSA_ERRCODE, GSA_UNDOC, GSA_SILENT };

#define IMG_LEN   (96u * 1024u + 123u)
#define MAXW      64
static uint8_t img[IMG_LEN + 16];
static uint32_t img_len;
static uint32_t pos, piece;
static uint32_t win[SWAL_NWIN][SWAL_WIN_LEN / 4];
static uint8_t carve[0x40000];
static uint8_t hdrbuf[4096];
static uint8_t chunk[5000];
static struct { int w; uint32_t off, v; } wl[MAXW];
static int nw, nreads, gsa_mode, gsa_err, corrupt, flushed, synced, flushed_at_bell, synced_at_bell, bell_ok, hdr_ok_at_bell;
static uint64_t cwrites, cwrite_bytes, cwrite_max;

static int
f_open(void *c) { (void)c; pos = 0; return 0; }

static int
f_next(void *c, uint8_t *buf, uint32_t cap, uint32_t *n)
{
	uint32_t k = piece < cap ? piece : cap;
	(void)c;
	if (k > img_len - pos) {
		k = img_len - pos;
	}
	memcpy(buf, img + pos, k);
	pos += k;
	*n = k;
	return 0;
}

static int
f_r32(void *c, int w, uint32_t off, uint32_t *v)
{
	(void)c;
	if (w < 0 || w >= SWAL_NWIN || (off & 3u) || off >= SWAL_WIN_LEN) {
		return 0;
	}
	nreads++;
	*v = win[w][off / 4];
	return 1;
}

static int
f_w32(void *c, int w, uint32_t off, uint32_t v)
{
	(void)c;
	if (w < 0 || w >= SWAL_NWIN || (off & 3u) || off >= SWAL_WIN_LEN || nw >= MAXW) {
		fails++;
		printf("FAIL bad write w=%d off=%#x\n", w, off);
		return 0;
	}
	wl[nw].w = w; wl[nw].off = off; wl[nw].v = v; nw++;
	if (w == SWAL_W_GSA && off == SWAL_INTCR0 && (v & 1u)) {
		win[w][SWAL_INTSR0 / 4] &= ~1u;
		win[w][SWAL_INTMSR0 / 4] &= ~1u;
		return 1;
	}
	win[w][off / 4] = v;
	if (w == SWAL_W_GSA && off == SWAL_INTGR1 && v == 1u) {
		uint32_t *g = win[SWAL_W_GSA];
		bell_ok = g[SWAL_SR(0) / 4] == 90u && g[SWAL_SR(1) / 4] == 4u && g[SWAL_SR(2) / 4] == 0x8a0000000ull % (1ull << 32) &&
		    g[SWAL_SR(3) / 4] == 8u && g[SWAL_SR(4) / 4] == 0x94000000u && g[SWAL_SR(5) / 4] == 0u && !(g[SWAL_INTMR0 / 4] & 1u);
		flushed_at_bell = flushed;
		synced_at_bell = synced;
		hdr_ok_at_bell = memcmp(hdrbuf, img, 4096) == 0 && memcmp(carve, img + 4096, img_len - 4096) == 0;
		if (gsa_mode != GSA_SILENT) {
			g[SWAL_SR(0) / 4] = gsa_mode == GSA_UNDOC ? 90u : (90u | 0x80000000u);
			g[SWAL_SR(1) / 4] = gsa_mode == GSA_ERRCODE ? (uint32_t)gsa_err : 0u;
			g[SWAL_SR(2) / 4] = 0;
			g[SWAL_INTSR0 / 4] |= 1u;
			g[SWAL_INTMSR0 / 4] = g[SWAL_INTSR0 / 4] & ~g[SWAL_INTMR0 / 4];
		}
	}
	return 1;
}

static uint64_t carve_len_cfg;

static int
f_cw(void *c, uint64_t off, const uint8_t *p, uint32_t n)
{
	(void)c;
	if (off + n > carve_len_cfg || off + n > sizeof(carve)) {
		fails++;
		printf("FAIL carveout write outside %#llx+%u\n", (unsigned long long)off, n);
		return 0;
	}
	memcpy(carve + off, p, n);
	cwrites++;
	cwrite_bytes += n;
	if (off + n > cwrite_max) {
		cwrite_max = off + n;
	}
	return 1;
}

static int
f_cr(void *c, uint64_t off, uint8_t *p, uint32_t n)
{
	(void)c;
	if (off + n > sizeof(carve)) {
		return 0;
	}
	memcpy(p, carve + off, n);
	if (corrupt && off == 0) {
		p[100] ^= 1;
	}
	return 1;
}

static void f_sync(void *c) { (void)c; synced++; }
static void f_flush(void *c) { (void)c; flushed++; }
static unsigned slept;
static void f_sleep(void *c, uint32_t ms) { (void)c; slept += ms; }
static void f_say(void *c, const char *w) { (void)c; (void)w; }

static void
put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* a synthetic image shaped like the stock one (docs/research/50 s.6.1 [HW from fw]) */
static void
make_image(void)
{
	uint32_t i;
	img_len = IMG_LEN;
	for (i = 0; i < img_len; i++) {
		img[i] = (uint8_t)(i * 7u + (i >> 9));
	}
	put32(img + 0x400, SWAL_AUTH_MAGIC); put32(img + 0x404, 1); put32(img + 0x408, 1); put32(img + 0x40C, img_len - 4096);
	put32(img + 4096, SWAL_SB_MAGIC); put32(img + 4096 + 4, 1); put32(img + 4096 + 12, 0x01400000u);
	put32(img + 4096 + 24, 0x1000); put32(img + 4096 + 28, 0x7000);          /* bootloader_offset, size */
	put32(img + 4096 + 32, 0x8000); put32(img + 4096 + 36, 0x2030);          /* uuid_table_offset, size */
}

static uint8_t idsha[32];

static void
calc_id(void)
{
	struct swal_sha s;
	swal_sha_init(&s);
	swal_sha_update(&s, img, img_len);
	swal_sha_final(&s, idsha);
}

static struct swal_res res;
static struct swal_bus bus;

static int
run(int mode, uint32_t req0, uint32_t pmu, uint64_t carve_len, const uint8_t *sha)
{
	memset(win, 0, sizeof(win));
	memset(carve, 0, sizeof(carve));
	memset(hdrbuf, 0, sizeof(hdrbuf));
	win[SWAL_W_PMU][SWAL_PMU_STAT / 4] = pmu;
	win[SWAL_W_REQ][0] = req0;
	win[SWAL_W_REQ][SWAL_REQ_STATUS / 4] = 1;
	win[SWAL_W_GSA][SWAL_INTMR0 / 4] = 0xFFFF;               /* gsa_mbox.c:234-235 init state */
	nw = nreads = 0; cwrites = cwrite_bytes = cwrite_max = 0; flushed = synced = 0; bell_ok = -1; slept = 0;
	flushed_at_bell = synced_at_bell = hdr_ok_at_bell = -1;
	gsa_mode = mode;
	carve_len_cfg = carve_len;
	bus = (struct swal_bus){ f_open, f_next, f_r32, f_w32, f_cw, f_cr, f_sync, f_flush, f_sleep, f_say, hdrbuf, 0x8a0000000ull,
		                 chunk, sizeof(chunk), carve_len, IMG_LEN, sha, 0 };
	return swal_run(&bus, &res);
}

static int
is_w(int i, int w, uint32_t off, uint32_t v)
{
	return i < nw && wl[i].w == w && wl[i].off == off && wl[i].v == v;
}

/* the GSA writes of one LOAD_AOC_FW_IMG, starting at wl[i] */
static int
gsa_seq(int i)
{
	return is_w(i, 2, 0x80, 90) && is_w(i + 1, 2, 0x84, 4) && is_w(i + 2, 2, 0x88, 0xa0000000u) && is_w(i + 3, 2, 0x8c, 8) &&
	       is_w(i + 4, 2, 0x90, 0x94000000u) && is_w(i + 5, 2, 0x94, 0) && is_w(i + 6, 2, 0x24, 1) && is_w(i + 7, 2, 0x28, 0xFFFE) &&
	       is_w(i + 8, 2, 0x40, 1);
}

static void
sha_vectors(void)
{
	static const struct { const char *m; const char *h; } v[] = {
		{ "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
		{ "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
		{ "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
	};
	unsigned i, k;
	for (i = 0; i < 3; i++) {
		struct swal_sha s;
		uint8_t o[32];
		char hx[65];
		swal_sha_init(&s);
		swal_sha_update(&s, (const uint8_t *)v[i].m, strlen(v[i].m));
		swal_sha_final(&s, o);
		for (k = 0; k < 32; k++) {
			sprintf(hx + 2 * k, "%02x", o[k]);
		}
		CHECK(strcmp(hx, v[i].h) == 0, "sha256 vector %u: %s", i, hx);
	}
	{       /* 1,000,000 x 'a' fed in odd pieces */
		struct swal_sha s;
		uint8_t a[997], o[32];
		uint32_t left = 1000000u, n;
		char hx[65];
		memset(a, 'a', sizeof(a));
		swal_sha_init(&s);
		for (; left; left -= n) {
			n = left < sizeof(a) ? left : (uint32_t)sizeof(a);
			swal_sha_update(&s, a, n);
		}
		swal_sha_final(&s, o);
		for (k = 0; k < 32; k++) {
			sprintf(hx + 2 * k, "%02x", o[k]);
		}
		CHECK(strcmp(hx, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0, "sha256 1M a: %s", hx);
	}
	{
		char hx[65];
		for (k = 0; k < 32; k++) {
			sprintf(hx + 2 * k, "%02x", swal_fw_sha[k]);
		}
		CHECK(strcmp(hx, "9be5662290149cb014d89b630d2a19b36e98a6580c7898eb9b5874cc28985743") == 0 && SWAL_FW_LEN == 15158976u,
		    "pinned identity differs from docs/research/50: %s", hx);
	}
	/* constants shared with tools/test_aoc_carveout.py and the cited sources */
	CHECK(SWAL_CARVE_PA == 0x94000000ull && SWAL_CARVE_LEN == 0x03000000u && SWAL_CMD_LOAD_AOC == 90u && SWAL_GSA_PA == 0x17C90000ull &&
	    SWAL_REQ_PA == 0x175D0000ull && SWAL_INTGR1 == 0x40u && SWAL_SR(5) == 0x94u, "constants");
}

static void
container_cases(void)
{
	static uint8_t c[256];
	const uint8_t *z, *sha;
	uint32_t zlen, usize, crc;
	memset(c, 0, sizeof(c));
	memcpy(c, "AOCFWZB1", 8); put32(c + 8, 1); put32(c + 12, 1);
	memcpy(c + 16, "AOCF", 4); put32(c + 20, 40 + 10); put32(c + 24, 32);
	put32(c + 32, 1234); put32(c + 36, 1); memset(c + 40, 0x5a, 32); memset(c + 72, 0x11, 10);
	crc = swal_crc32(c + 32, 50);
	put32(c + 28, crc);
	CHECK(swal_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, "crc32 check value");
	CHECK(swal_container(c, 88, &z, &zlen, &usize, &sha) == 0 && zlen == 10 && usize == 1234 && z == c + 72 && sha == c + 40, "container ok");
	c[72] ^= 1;
	CHECK(swal_container(c, 88, &z, &zlen, &usize, &sha) == -5, "container crc");
	c[72] ^= 1;
	CHECK(swal_container(c, 81, &z, &zlen, &usize, &sha) == -4, "container truncated");
	c[0] = 'X';
	CHECK(swal_container(c, 88, &z, &zlen, &usize, &sha) == -2, "container magic");
}

int
main(void)
{
	uint8_t badsha[32];
	int rc, i;

	sha_vectors();
	container_cases();
	make_image();
	calc_id();
	memcpy(badsha, idsha, 32);
	badsha[31] ^= 0x80;

	/* 1. good image, aoc_req found 1 (as on hardware, A4), source pieces cross the 4096 boundary */
	piece = 1000;
	rc = run(GSA_OK, 1, 1, 0x40000, idsha);
	CHECK(rc == SWAL_OK && res.stage == SWAL_OK, "good: rc %d", rc);
	CHECK(nw == 12 && is_w(0, 1, 0, 1) && gsa_seq(1) && is_w(10, 2, 0x24, 1) && is_w(11, 2, 0x28, 0xFFFF), "good: writes (%d)", nw);
	CHECK(bell_ok == 1 && flushed_at_bell == 1 && synced_at_bell == 1 && hdr_ok_at_bell == 1, "good: state at the doorbell");
	CHECK(cwrite_bytes == IMG_LEN - 4096 && cwrite_max == IMG_LEN - 4096, "good: carveout bytes %llu", (unsigned long long)cwrite_bytes);
	CHECK(res.len1 == IMG_LEN && res.len2 == IMG_LEN && res.req_found == 1 && res.rsp_err == 0 && res.auth_gen == 1 &&
	    res.sb_image_size == 0x01400000u, "good: results");

	/* 1b. good image, other piece sizes (chunk-sized, 1 byte) give the same writes */
	piece = sizeof(chunk);
	CHECK(run(GSA_OK, 1, 1, 0x40000, idsha) == SWAL_OK && nw == 12, "good, big pieces");
	piece = 4096;
	CHECK(run(GSA_OK, 1, 1, 0x40000, idsha) == SWAL_OK && nw == 12, "good, 4096 pieces");

	/* 2. wrong sha: nothing touched at all, not even a read */
	piece = 1000;
	rc = run(GSA_OK, 1, 1, 0x40000, badsha);
	CHECK(rc == SWAL_E_IDENTITY && nw == 0 && nreads == 0 && cwrites == 0, "wrong sha: rc %d nw %d reads %d", rc, nw, nreads);

	/* 2b. wrong length (one byte short, one byte long) */
	img_len = IMG_LEN - 1;
	rc = run(GSA_OK, 1, 1, 0x40000, idsha);
	CHECK(rc == SWAL_E_IDENTITY && nw == 0 && nreads == 0 && cwrites == 0, "short image: rc %d", rc);
	img_len = IMG_LEN + 1;
	rc = run(GSA_OK, 1, 1, 0x40000, idsha);
	CHECK(rc == SWAL_E_IDENTITY && nw == 0 && nreads == 0 && cwrites == 0, "long image: rc %d", rc);
	img_len = IMG_LEN;

	/* 3. bad header: superbin magic; auth generation; uuid region outside: only aoc_req := 1 (and its restore if found 0) */
	for (i = 0; i < 3; i++) {
		make_image();
		if (i == 0) {
			img[4096] ^= 1;
		} else if (i == 1) {
			put32(img + 0x404, 3);
		} else {
			put32(img + 4096 + 32, IMG_LEN - 4096 - 0x2030);   /* start + len == size - 4096: not < (region_is_in_firmware) */
		}
		calc_id();
		rc = run(GSA_OK, 1, 1, 0x40000, idsha);
		CHECK(rc == SWAL_E_HEADER && nw == 1 && is_w(0, 1, 0, 1) && cwrites == 0, "bad header %d: rc %d nw %d", i, rc, nw);
		rc = run(GSA_OK, 0, 1, 0x40000, idsha);
		CHECK(rc == SWAL_E_HEADER && nw == 2 && is_w(0, 1, 0, 1) && is_w(1, 1, 0, 0) && cwrites == 0, "bad header %d, req found 0: nw %d", i, nw);
	}
	make_image();
	calc_id();

	/* 4. body larger than the carveout: nothing touched */
	rc = run(GSA_OK, 1, 1, IMG_LEN - 4096 - 1, idsha);
	CHECK(rc == SWAL_E_TOO_BIG && nw == 0 && nreads == 0 && cwrites == 0, "too big: rc %d", rc);
	/* 4b. body exactly the carveout size is allowed */
	CHECK(run(GSA_OK, 1, 1, IMG_LEN - 4096, idsha) == SWAL_OK, "exact fit");

	/* 5. AOC power domain off: one read, no write */
	rc = run(GSA_OK, 1, 0, 0x40000, idsha);
	CHECK(rc == SWAL_E_PD_OFF && nw == 0 && nreads == 1 && cwrites == 0, "pd off: rc %d", rc);

	/* 6. read-back mismatch: copy happened, then no GSA write; aoc_req restored when found 0 */
	corrupt = 1;
	rc = run(GSA_OK, 0, 1, 0x40000, idsha);
	CHECK(rc == SWAL_E_READBACK && nw == 2 && is_w(0, 1, 0, 1) && is_w(1, 1, 0, 0), "readback: rc %d nw %d", rc, nw);
	corrupt = 0;

	/* 7. GSA error codes: the full sequence once, the stock post-response clear/mask, then nothing */
	{
		static const uint32_t errs[] = { 1, 2, 3, 4, 5, 6, 128, 129, 130, 131 };
		unsigned k;
		for (k = 0; k < sizeof(errs) / sizeof(errs[0]); k++) {
			gsa_err = (int)errs[k];
			rc = run(GSA_ERRCODE, 1, 1, 0x40000, idsha);
			CHECK(rc == SWAL_E_GSA_ERR && res.rsp_err == errs[k] && nw == 12 && gsa_seq(1) && is_w(10, 2, 0x24, 1) &&
			    is_w(11, 2, 0x28, 0xFFFF), "gsa err %u: rc %d nw %d", errs[k], rc, nw);
		}
		gsa_err = 77;   /* not in gsa_mbox.c:345-360 */
		rc = run(GSA_ERRCODE, 1, 1, 0x40000, idsha);
		CHECK(rc == SWAL_E_GSA_UNDOC && nw == 12, "gsa err 77: rc %d", rc);
	}
	/* 8. reply without the response bit */
	rc = run(GSA_UNDOC, 1, 1, 0x40000, idsha);
	CHECK(rc == SWAL_E_GSA_UNDOC && nw == 12, "gsa undocumented: rc %d", rc);

	/* 9. mailbox timeout: bounded, the mask restored as found, nothing else */
	rc = run(GSA_SILENT, 1, 1, 0x40000, idsha);
	CHECK(rc == SWAL_E_GSA_TIMEOUT && nw == 11 && gsa_seq(1) && is_w(10, 2, 0x28, 0xFFFF) && slept == SWAL_GSA_WAIT_MS,
	    "timeout: rc %d nw %d slept %u", rc, nw, slept);

	/* 10. header buffer above the 36-bit GSA DMA limit: no GSA write */
	bus.hdr_pa = 0;
	{
		struct swal_bus b2;
		run(GSA_OK, 1, 1, 0x40000, idsha);      /* reset the fake */
		nw = 0;
		memset(win, 0, sizeof(win));
		win[SWAL_W_PMU][SWAL_PMU_STAT / 4] = 1;
		win[SWAL_W_REQ][0] = 1;
		b2 = bus;
		b2.hdr_pa = 1ull << 36;
		rc = swal_run(&b2, &res);
		CHECK(rc == SWAL_E_HDR_PA && nw == 1 && is_w(0, 1, 0, 1), "hdr pa: rc %d nw %d", rc, nw);
	}

	printf("%s aoc_load_host_test (%d failures)\n", fails ? "RESULT: FAIL" : "RESULT: PASS", fails);
	return fails ? 1 : 0;
}
