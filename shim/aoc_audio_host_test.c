/* Host test of the AOC audio path open (xnu patch 0166, bsd/dev/sweet_audio.h, sweetaoc=4, plan step A9a) against a fake AOC queue peer in a fake
 * IPC window, and of the CS35L41 HALO file parser (bsd/dev/sweet_cs_dsp.h, A9c parser only) on synthetic files and, when present, on the stock
 * files in $SWEET_CS35L41_FW (default ~/pixelios-artifacts/aoc/cs35l41; skipped if absent, never in git).
 * AOC cases: good open + close (exact bytes, ids 233 / 271 / 271, cntr 0..2, one doorbell per message, counters), a stale reply drained,
 * BIND2 rejected (reply != 0: stop, undo sent), a 4-byte AOC error code, a timeout (<= 500 ms), an unknown reply id, a ring service refused,
 * a full DOWN queue (nothing written), counters corrupt.
 * Patch 0167 (sweetaoc=5): the play open (audio_playback0 ring zeroed + reset, EP_SETUP 206 and SOURCE 201 bytes), zero feeds (exact amount, cap,
 * wrap, push-ring doorbell), the close order (SOURCE on 0, then BIND2 0), each stop inside the play open (SOURCE rejected or unanswered: stop sent;
 * EP_SETUP or BIND2 rejected: no SOURCE, bind undone) and the refusals (queue type, size, window, counters, indexes, overlaps: no zero write).
 * Patch 0168: a reply with another id but type 0 and the sent length is accepted (stock never reads the id); EP_SETUP answered with id 288 as
 * on the phone (run a9c): SOURCE on sent, then SOURCE off + BIND2 0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sweet_audio.h"
#include "sweet_cs_dsp.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t ipc[SWAU_IPC_LEN];
#define SO   0xC0u          /* services_offset (A8: 0xc0) */
#define SS   92u            /* service_size (A8: 92) */
#define IDX  6u             /* audio_output_control (A8: 6) */
#define HDR  (SO + IDX * SS)
#define UPO  0x10000u
#define DNO  0x20000u
#define SLOT 128u
#define NSL  4u

static void w32(uint32_t o, uint32_t v) { ipc[o] = (uint8_t)v; ipc[o + 1] = (uint8_t)(v >> 8); ipc[o + 2] = (uint8_t)(v >> 16); ipc[o + 3] = (uint8_t)(v >> 24); }
static uint32_t r32(uint32_t o) { return ipc[o] | ipc[o + 1] << 8 | ipc[o + 2] << 16 | (uint32_t)ipc[o + 3] << 24; }
#define RG(d, f) (HDR + 36u + 28u * (d) + 4u * (f))      /* f: 0 offset 1 size 2 slots 3 tx 4 rx 5 wp 6 rp */

static int mode;            /* peer: 0 echo reply 0, 1 reject BIND2 bind 1, 2 error code on 233, 3 silent, 4 wrong id, 5 reply len differs,
                             * 0167: 6 silent on SOURCE on 1, 7 reject SOURCE on 1, 8 reject EP_SETUP */
static int nbell, nsync, nwrite_bad, stale;
static uint8_t sent[8][32];
static uint32_t sent_len[8], nsent, now;

static void
setup(uint32_t flags)
{
	memset(ipc, 0, sizeof(ipc));
	memcpy(ipc + HDR, "audio_output_control", 20);
	w32(HDR + 32u, flags);
	w32(RG(0, 0), UPO); w32(RG(0, 1), SLOT); w32(RG(0, 2), NSL);
	w32(RG(1, 0), DNO); w32(RG(1, 1), SLOT); w32(RG(1, 2), NSL);
	nbell = nsync = nwrite_bad = 0; nsent = 0; now = 0; mode = 0; stale = 0;
	memset(sent, 0, sizeof(sent)); memset(sent_len, 0, sizeof(sent_len));
}

static void
up_push(const uint8_t *m, uint32_t len)
{
	uint32_t tx = r32(RG(0, 3)), slot = UPO + SLOT * (tx % NSL);

	w32(slot, len); w32(slot + 4u, 0);
	memcpy(ipc + slot + 8u, m, len);
	w32(RG(0, 3), tx + 1u);
	w32(RG(0, 5), (r32(RG(0, 5)) + 1u) % SLOT);
}

/* the peer: on a doorbell, consume every DOWN message and answer per mode */
static void
peer(void)
{
	while (r32(RG(1, 3)) != r32(RG(1, 4))) {
		uint32_t rx = r32(RG(1, 4)), slot = DNO + SLOT * (rx % NSL), len = r32(slot) & 0xFFFFu;
		uint8_t m[64];

		if (r32(slot + 4u) != 0u) nwrite_bad++;
		memcpy(m, ipc + slot + 8u, len);
		if (nsent < 8u) { memcpy(sent[nsent], m, len < 32u ? len : 32u); sent_len[nsent++] = len; }
		w32(RG(1, 4), rx + 1u);
		w32(RG(1, 6), (r32(RG(1, 6)) + 1u) % SLOT);
		uint16_t id = (uint16_t)(m[4] | m[5] << 8);
		if (mode == 3) continue;
		if (mode == 6 && id == 201u && m[9] == 1u) continue;
		if ((mode == 7 && id == 201u && m[9] == 1u) || (mode == 8 && id == 206u)) { m[6] = 0xEA; m[7] = 0xFF; }
		if (mode == 2 && id == 233u) { uint8_t e[4] = { 0xEA, 0xFF, 0xFF, 0xFF }; up_push(e, 4); continue; }
		if (mode == 1 && id == 271u && m[10] == 1u) { m[6] = 0xEA; m[7] = 0xFF; }
		if (mode == 4) { m[4] = 0x99; }
		if (mode == 9 && id == 206u) { m[4] = 0x20; m[5] = 0x01; }   /* id 288, as observed on hardware (run a9c) */
		if (id == 233u) m[8] = 1u;      /* mode PLAYBACK */
		up_push(m, mode == 5 ? len + 1u : len);
	}
}

static int f_ir32(void *c, uint32_t o, uint32_t *v) { (void)c; if (o & 3u || o > SWAU_IPC_LEN - 4u) return 0; *v = r32(o); return 1; }
static int f_iw32(void *c, uint32_t o, uint32_t v) { (void)c; if (o & 3u || o > SWAU_IPC_LEN - 4u) return 0; w32(o, v); return 1; }
static void f_sync(void *c) { (void)c; nsync++; }
static int f_bell(void *c, uint32_t irq) { (void)c; if (irq != 0u) return 0; nbell++; peer(); return 1; }
static void f_sleep(void *c, uint32_t ms) { (void)c; now += ms; }
static void f_say(void *c, const char *w) { (void)c; (void)w; }
static const struct swau_bus B = { f_ir32, f_iw32, f_sync, f_bell, f_sleep, f_say, NULL };

static void
t_good(int with_stale)
{
	struct swau_res r;
	int rc;

	setup(0);
	if (with_stale) { uint8_t old[11] = { 0, 9, 11, 0, 0x0F, 0x01, 0, 0, 0, 0, 1 }; up_push(old, 11); up_push(old, 11); }
	rc = swau_open(&B, &r, SO, SS, IDX);
	CHECK(rc == SWAU_OK, "good open rc %d", rc);
	CHECK(r.bound == 1 && r.dsp_mode == 1u && r.nx == 2u, "bound %u mode %u nx %u", r.bound, r.dsp_mode, r.nx);
	CHECK(!with_stale || r.x[0].drained == 2u, "stale drained %u", r.x[0].drained);
	rc = swau_close(&B, &r);
	CHECK(rc == SWAU_OK && r.unbound == 1, "close rc %d", rc);
	CHECK(swau_close(&B, &r) == SWAU_OK && nsent == 3u, "close twice sends once (%u)", nsent);
	{
		static const uint8_t e0[9] = { 0, 0, 9, 0, 233, 0, 0, 0, 0 }, e1[11] = { 0, 1, 11, 0, 0x0F, 0x01, 0, 0, 0, 0, 1 },
		    e2[11] = { 0, 2, 11, 0, 0x0F, 0x01, 0, 0, 0, 0, 0 };
		CHECK(sent_len[0] == 9u && !memcmp(sent[0], e0, 9), "GET_DSP_STATE bytes");
		CHECK(sent_len[1] == 11u && !memcmp(sent[1], e1, 11), "BIND2 bind 1 bytes");
		CHECK(sent_len[2] == 11u && !memcmp(sent[2], e2, 11), "BIND2 bind 0 bytes");
	}
	CHECK(nbell == 3 && nwrite_bad == 0, "one doorbell per message (%d)", nbell);
	CHECK(r32(RG(1, 3)) == 3u && r32(RG(1, 5)) == 3u && r32(RG(0, 4)) == (with_stale ? 5u : 3u), "counters");
	printf("ok: good open/close%s\n", with_stale ? " (2 stale replies drained)" : "");
}

static void
t_stop(int m, int expect, int bound, uint32_t nmsg, const char *what)
{
	struct swau_res r;
	int rc;

	setup(0);
	mode = m;
	rc = swau_open(&B, &r, SO, SS, IDX);
	CHECK(rc == expect, "%s: rc %d want %d", what, rc, expect);
	CHECK((int)r.bound == bound, "%s: bound %u", what, r.bound);
	swau_close(&B, &r);
	CHECK(nsent == nmsg, "%s: %u messages (want %u)", what, nsent, nmsg);
	if (m == 3) CHECK(r.x[0].waited_ms == SWAU_REPLY_MS && now == SWAU_REPLY_MS, "%s: bounded wait %u", what, r.x[0].waited_ms);
	if (m == 2) CHECK(r.x[0].aoc_err == 0xFFFFFFEAu, "%s: error code", what);
	if (m == 1) CHECK(r.x[1].reply == -22 && sent[2][10] == 0u, "%s: reply %d, undo bind 0", what, r.x[1].reply);
	printf("ok: %s\n", what);
}

static void
t_refuse(void)
{
	struct swau_res r;

	setup(0x10000u);        /* a ring */
	CHECK(swau_open(&B, &r, SO, SS, IDX) == SWAU_E_SVC && nsent == 0u && nbell == 0, "ring service refused, nothing written");
	setup(0);
	w32(RG(1, 3), NSL);     /* DOWN full: tx - rx = slots */
	CHECK(swau_open(&B, &r, SO, SS, IDX) == SWAU_E_FULL && nbell == 0 && r32(RG(1, 3)) == NSL && !r.bound, "full queue: nothing written");
	setup(0);
	w32(RG(0, 3), 100u);    /* UP tx - rx > slots */
	CHECK(swau_open(&B, &r, SO, SS, IDX) == SWAU_E_CORRUPT && nbell == 0, "corrupt counters: stop");
	setup(0);
	w32(RG(1, 0), SWAU_IPC_LEN - 64u);
	CHECK(swau_open(&B, &r, SO, SS, IDX) == SWAU_E_SVC && nbell == 0, "region outside the window refused");
	printf("ok: refusals (ring, full, corrupt, outside)\n");
}

/* ---- patch 0167 (sweetaoc=5): audio_playback0 ring + EP_SETUP + SOURCE on / off ---- */
#define PBI   8u                                /* audio_playback0 index in the fake table */
#define NSVC  12u
#define PHDR  (SO + PBI * SS)
#define PG(f) (PHDR + 36u + 28u * 1u + 4u * (f))  /* its AOC_DOWN region: 0 offset 1 size 2 slots 3 tx 4 rx 5 wp 6 rp */
#define PBO   0x40000u
#define PBS   0x800u

static void
pb_setup(uint32_t flags, uint32_t wp)
{
	memcpy(ipc + PHDR, "audio_playback0", 15);
	w32(PHDR + 32u, flags);
	w32(PG(0), PBO); w32(PG(1), PBS); w32(PG(2), 1u);
	w32(PG(3), 1000u + wp); w32(PG(4), 1000u + wp); w32(PG(5), wp);   /* drained ring, earlier use */
	memset(ipc + PBO, 0xAA, PBS);           /* stale non-zero samples */
	memset(ipc + PBO + PBS, 0x55, 64);      /* the bytes right after the ring: never touched */
}

static int
pb_all_zero(void)
{
	for (uint32_t i = 0; i < PBS; i++)
		if (ipc[PBO + i]) return 0;
	for (uint32_t i = 0; i < 64u; i++)
		if (ipc[PBO + PBS + i] != 0x55u) return 0;
	return 1;
}

static void
t_play_good(uint32_t flags)
{
	struct swau_res r;
	int rc, bells;
	uint32_t tx0;

	setup(0);
	pb_setup(flags, 0x100u);
	rc = swau_play_open(&B, &r, SO, SS, IDX, PBI, NSVC);
	CHECK(rc == SWAU_OK && r.started && r.ep_set && r.bound && r.nx == 4u, "play open rc %d started %u", rc, r.started);
	CHECK(pb_all_zero() && r.pb.zeroed && r.pb.reset, "ring zeroed (stale 0xAA gone), bytes after it untouched");
	CHECK(r32(PG(3)) == 1000u + 0x100u + (PBS - 0x100u) && r32(PG(5)) == 0u, "stock reset: tx += size - wp, wp = 0 (tx %u wp %u)", r32(PG(3)), r32(PG(5)));
	{
		static const uint8_t ep[28] = { 0, 2, 28, 0, 206, 0, 0, 0, 0, 0, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0, 0, 2, 1, 5, 1, 0, 0, 1, 0 };
		static const uint8_t s1[10] = { 0, 3, 10, 0, 201, 0, 0, 0, 0, 1 };
		CHECK(sent_len[2] == 28u && !memcmp(sent[2], ep, 28), "EP_SETUP bytes");
		CHECK(sent_len[3] == 10u && !memcmp(sent[3], s1, 10), "SOURCE on bytes");
	}
	/* the reset published size - wp; the AOC consumed 300 more: one feed fills the 256 + 300 free bytes with zeros */
	tx0 = r32(PG(3));
	w32(PG(4), r32(PG(4)) + 300u);
	memset(ipc + PBO, 0x11, 4);             /* would only be overwritten by a zero */
	bells = nbell;
	swau_feed(&B, &r);
	CHECK(r32(PG(3)) == tx0 + 556u && r32(PG(5)) == 556u && r.pb.fed == 556u && r.pb.feed_err == 0u, "feed: the 256 never-published + 300 consumed: tx +556, wp 556 (fed %u)", r.pb.fed);
	CHECK(pb_all_zero(), "feed writes zeros only");
	CHECK(nbell - bells == ((flags & SWAU_RING_PUSH) ? 1 : 0), "doorbell only for a push ring");
	swau_feed(&B, &r);
	CHECK(r.pb.fed == 556u, "nothing consumed: nothing fed");
	w32(PG(4), r32(PG(4)) + 1500u);         /* 1500 free: capped */
	swau_feed(&B, &r);
	CHECK(r.pb.fed == 556u + SWAU_FEED_MAX && r32(PG(5)) == 556u + SWAU_FEED_MAX && pb_all_zero(), "feed capped at SWAU_FEED_MAX, zeros");
	swau_feed(&B, &r);                      /* the remaining 476 free bytes: wraps to wp 8 */
	CHECK(r.pb.fed == 556u + SWAU_FEED_MAX + 476u && r32(PG(5)) == 8u && r32(PG(3)) - r32(PG(4)) == PBS && pb_all_zero(), "feed wraps (wp %u), ring full, zeros", r32(PG(5)));
	rc = swau_close(&B, &r);
	CHECK(rc == SWAU_OK && r.stopped && r.unbound && nsent == 6u, "close: SOURCE off + BIND2 0 (%u messages)", nsent);
	{
		static const uint8_t s0[10] = { 0, 4, 10, 0, 201, 0, 0, 0, 0, 0 }, b0[11] = { 0, 5, 11, 0, 0x0F, 0x01, 0, 0, 0, 0, 0 };
		CHECK(!memcmp(sent[4], s0, 10) && !memcmp(sent[5], b0, 11), "SOURCE on 0 then BIND2 bind 0, in that order");
	}
	CHECK(r.consumed == 1800u, "consumed %u", r.consumed);
	tx0 = r32(PG(3));
	swau_feed(&B, &r);
	CHECK(r32(PG(3)) == tx0 && swau_close(&B, &r) == SWAU_OK && nsent == 6u, "after the stop: no feed, close twice sends nothing");
	printf("ok: play open / feed / close%s (zeros only, SOURCE off then BIND2 0)\n", (flags & SWAU_RING_PUSH) ? " (push ring)" : "");
}

/* a stop inside the play open: what was sent, what the close undoes */
static void
t_play_stop(int m, int expect, uint32_t started, uint32_t nmsg, const char *what)
{
	struct swau_res r;
	int rc;

	setup(0);
	pb_setup(0x10000u, 0u);
	mode = m;
	rc = swau_play_open(&B, &r, SO, SS, IDX, PBI, NSVC);
	CHECK(rc == expect && r.started == started, "%s: rc %d want %d started %u", what, rc, expect, r.started);
	mode = 0;
	swau_close(&B, &r);
	CHECK(nsent == nmsg, "%s: %u messages (want %u)", what, nsent, nmsg);
	if (started)
		CHECK(sent_len[4] == 10u && sent[4][4] == 201u && sent[4][9] == 0u && sent[5][4] == 0x0Fu && sent[5][10] == 0u, "%s: SOURCE off then BIND2 0", what);
	else
		CHECK(sent[nmsg - 1u][4] == 0x0Fu && sent[nmsg - 1u][10] == 0u, "%s: BIND2 0 last, no SOURCE off", what);
	if (m != 1)
		CHECK(pb_all_zero(), "%s: ring zeros", what);
	printf("ok: %s\n", what);
}

static void
t_play_refuse(void)
{
	struct swau_res r;

	setup(0); pb_setup(0u, 0u);             /* a queue, not a ring */
	CHECK(swau_play_open(&B, &r, SO, SS, IDX, PBI, NSVC) == SWAU_E_SVC && nsent == 0u && nbell == 0 && ipc[PBO] == 0xAAu, "queue-type playback0 refused, nothing sent");
	setup(0); pb_setup(0x10000u, 0u); w32(PG(1), SWAU_RING_MAX + 4u);
	CHECK(swau_play_open(&B, &r, SO, SS, IDX, PBI, NSVC) == SWAU_E_SVC && nsent == 0u, "ring bigger than SWAU_RING_MAX refused");
	setup(0); pb_setup(0x10000u, 0u); w32(PG(0), SWAU_IPC_LEN - 0x400u);
	CHECK(swau_play_open(&B, &r, SO, SS, IDX, PBI, NSVC) == SWAU_E_SVC && nsent == 0u, "ring past the window refused");
	setup(0); pb_setup(0x10000u, 0u); w32(PG(3), 1000u + PBS + 4u);
	CHECK(swau_play_open(&B, &r, SO, SS, IDX, PBI, NSVC) == SWAU_E_CORRUPT && nsent == 0u, "ring counters corrupt refused");
	setup(0); pb_setup(0x10000u, 0u);
	CHECK(swau_play_open(&B, &r, SO, SS, IDX, IDX, NSVC) == SWAU_E_ARGS && nsent == 0u, "playback index = control index refused");
	CHECK(swau_play_open(&B, &r, SO, SS, IDX, PBI, PBI) == SWAU_E_ARGS && nsent == 0u, "playback index outside the table refused");
	setup(0); pb_setup(0x10000u, 0u); w32(PG(0), DNO);      /* the ring over the control DOWN queue */
	CHECK(swau_play_open(&B, &r, SO, SS, IDX, PBI, NSVC) == SWAU_E_SVC && !r.pb.zeroed && r.bound, "ring over the control queue: refused before any zero write");
	swau_close(&B, &r);
	CHECK(nsent == 3u && sent[2][10] == 0u && !r.stopped, "  ... and the bind is undone, no SOURCE off");
	setup(0); pb_setup(0x10000u, 0u); w32(PG(0), SO + 4u * SS);   /* over the service headers */
	CHECK(swau_play_open(&B, &r, SO, SS, IDX, PBI, NSVC) == SWAU_E_SVC && !r.pb.zeroed, "ring over the service headers refused");
	printf("ok: play refusals (queue type, size, window, counters, indexes, overlaps)\n");
}

/* ---- the HALO file parser ---- */
static uint8_t *
slurp(const char *dir, const char *name, size_t *n)
{
	char p[512];
	FILE *f;
	uint8_t *b;
	long l;

	snprintf(p, sizeof(p), "%s/%s", dir, name);
	if (!(f = fopen(p, "rb"))) return NULL;
	fseek(f, 0, SEEK_END); l = ftell(f); fseek(f, 0, SEEK_SET);
	b = malloc((size_t)l + 1u);
	*n = fread(b, 1, (size_t)l, f);
	fclose(f);
	return b;
}

static struct swcd_file F;

static void
t_parser_synthetic(void)
{
	uint8_t w[40 + 8 + 12 + 8 + 5];
	size_t n = sizeof(w);

	memset(w, 0, sizeof(w));
	memcpy(w, "WMFW", 4); w[4] = 40; w[8] = 0; w[9] = 3; w[10] = 4; w[11] = 3;
	/* region: XM packed at offset 4, 12 bytes; region: PM packed offset 2, 5 bytes */
	w[40] = 4; w[43] = 0x11; w[44] = 12;
	w[60] = 2; w[63] = 0x10; w[64] = 5;
	CHECK(swcd_wmfw(w, n, &F) == SWCD_OK && F.nitems == 2u && F.it[0].reg == 0x0200000Cu && F.it[1].reg == 0x0380000Au && F.write_bytes == 17u,
	      "synthetic wmfw regs %#x %#x", F.it[0].reg, F.it[1].reg);
	w[64] = 6;
	CHECK(swcd_wmfw(w, n, &F) == SWCD_E_OVERFLOW, "region past the end refused");
	w[64] = 5; w[11] = 2;
	CHECK(swcd_wmfw(w, n, &F) == SWCD_E_VERSION, "ver 2 refused");
	w[11] = 3; w[10] = 2;
	CHECK(swcd_wmfw(w, n, &F) == SWCD_E_CORE, "ADSP2 core refused");
	w[10] = 4; w[4] = 44;
	CHECK(swcd_wmfw(w, n, &F) == SWCD_E_LEN, "header len refused");
	printf("ok: parser synthetic\n");
}

static void
t_parser_stock(void)
{
	const char *dir = getenv("SWEET_CS35L41_FW");
	char def[512];
	static const char *const bins[2] = { "cs35l41-dsp1-spk-prot.bin", "R-cs35l41-dsp1-spk-prot.bin" };
	uint8_t *d;
	size_t n = 0;
	uint32_t i, pm = 0, xm = 0, ym = 0;

	if (!dir) {
		snprintf(def, sizeof(def), "%s/pixelios-artifacts/aoc/cs35l41", getenv("HOME") ? getenv("HOME") : ".");
		dir = def;
	}
	d = slurp(dir, "cs35l41-dsp1-spk-prot.wmfw", &n);
	if (!d) {
		printf("skip: stock wmfw not present in %s\n", dir);
		return;
	}
	CHECK(n == 33988u, "wmfw length %zu (pinned 33988)", n);
	CHECK(swcd_wmfw(d, n, &F) == SWCD_OK, "stock wmfw parses");
	for (i = 0; i < F.nitems; i++) {
		pm += F.it[i].type == SWCD_PM_PACKED ? F.it[i].len : 0u;
		xm += F.it[i].type == SWCD_XM_PACKED ? F.it[i].len : 0u;
		ym += F.it[i].type == SWCD_YM_PACKED ? F.it[i].len : 0u;
		CHECK(!F.it[i].reg || (F.it[i].reg >= 0x02000000u && F.it[i].reg < 0x04000000u), "region %u reg %#x inside the DSP map", i, F.it[i].reg);
	}
	printf("ok: stock wmfw rev %#x core %u ver %u sizes xm %u ym %u pm %u: %u regions (%u text), %u bytes to write (pm %u xm %u ym %u), odd lengths %u\n",
	       F.rev, F.core, F.ver, F.sizes[0], F.sizes[1], F.sizes[2], F.nitems, F.text_items, F.write_bytes, pm, xm, ym, F.odd_len);
	free(d);
	for (i = 0; i < 2u; i++) {
		d = slurp(dir, bins[i], &n);
		CHECK(d != NULL, "%s present", bins[i]);
		if (!d) continue;
		CHECK(swcd_bin(d, n, &F) == SWCD_OK, "%s parses", bins[i]);
		printf("ok: stock %s rev %#x ver %#x: %u blocks (%u text, %u alg-relative), %u bytes to write, odd %u\n", bins[i], F.rev, F.ver, F.nitems,
		       F.text_items, F.alg_relative, F.write_bytes, F.odd_len);
		free(d);
	}
}

int
main(void)
{
	t_good(0);
	t_good(1);
	t_stop(1, SWAU_E_REJECTED, 1, 3u, "BIND2 rejected (reply -22): stop, undo sent");
	t_stop(2, SWAU_E_AOCERR, 0, 1u, "4-byte AOC error on GET_DSP_STATE: stop, no BIND2");
	t_stop(3, SWAU_E_TIMEOUT, 0, 1u, "no reply: timeout after 500 ms, no BIND2");
	t_stop(4, SWAU_OK, 1, 3u, "reply with another id, same type + length: accepted as stock (0168), bind undone on close");
	t_stop(5, SWAU_E_UNKNOWN, 0, 1u, "reply of another length: unknown, stop");
	t_refuse();
	t_play_good(0x10000u);
	t_play_good(0x10000u | SWAU_RING_PUSH);
	t_play_stop(7, SWAU_E_REJECTED, 1u, 6u, "SOURCE on rejected: stop, SOURCE off + BIND2 0 sent");
	t_play_stop(6, SWAU_E_TIMEOUT, 1u, 6u, "SOURCE on unanswered (500 ms): stop, SOURCE off + BIND2 0 sent");
	t_play_stop(8, SWAU_E_REJECTED, 0u, 4u, "EP_SETUP rejected: no SOURCE, BIND2 0 sent");
	t_play_stop(9, SWAU_OK, 1u, 6u, "EP_SETUP answered with id 288 (run a9c): accepted, SOURCE on sent, then SOURCE off + BIND2 0");
	t_play_stop(1, SWAU_E_REJECTED, 0u, 3u, "BIND2 rejected in the play open: no EP_SETUP, BIND2 0 sent");
	t_play_refuse();
	t_parser_synthetic();
	t_parser_stock();
	printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
