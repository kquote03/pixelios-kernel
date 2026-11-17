/* Host test of the AOC start + service discovery (xnu patch 0165, bsd/dev/sweet_aoc_start.h, sweetaoc=3, plan step A8)
 * against a fake SoC (chipid, aoc_req, SSMT, AOC mailbox, SysMMU), a fake TIPC peer (the hwmgr sequence result) and a fake
 * AOC firmware that writes the control block + service table into the IPC window. Cases: good start with N services incl.
 * audio_output_control, START error code, gate (not LOADED, START never sent), undocumented state, no ready, malformed
 * service entries (unterminated name, duplicate, bad character, zero services, table outside the window), no
 * audio_output_control, chipid mismatch, aoc_req off, param read-back mismatch. Every case checks the exact register
 * writes: the 64 SSMT writes and their restore only where the AOC was not started, aoc_req := 0 only when online. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sweet_aoc_start.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MAXW 512
static uint32_t win[SWAS_NWIN][0x5000 / 4];
static uint8_t ipc[SWAS_IPC_LEN];
static struct { int w; uint32_t off, v; } wl[MAXW];
static int nw, mapped, maps, tipc_calls, synced, synced_before_tipc, param_ok_at_tipc, ssmt_at_tipc, corrupt_param;
static uint64_t map_pa;
static uint32_t now_ms, ready_at, slept;
static struct swas_tipc tscript;
static int fw_mode;     /* 0 none, 1 good table, 2 unterminated, 3 dup, 4 bad char, 5 zero svc, 6 outside, 7 no ctl */
static const char *svc5[] = { "audio_output_control", "audio_playback0", "audio_capture0", "sensor_ctl", "logging" };

static void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void
fw_write_table(void)
{
	uint32_t n = 5, size = 128, off = 0x1000, i;

	memset(ipc, 0, 0x4000);
	wr32le(ipc + 4, 0x1234);
	memcpy(ipc + 36, "aoc-fake-1", 10);
	if (fw_mode == 5) n = 0;
	if (fw_mode == 6) off = SWAS_IPC_LEN - 200;
	wr32le(ipc + 24, n);
	wr32le(ipc + 28, size);
	wr32le(ipc + 32, off);
	for (i = 0; i < 5 && off + (i + 1) * size <= SWAS_IPC_LEN; i++) {
		uint8_t *s = ipc + off + i * size;
		const char *nm = svc5[i];
		if (fw_mode == 7 && i == 0) nm = "audio_output_contro";
		strcpy((char *)s, nm);
		wr32le(s + 32, i == 1 ? 0x00010000u | (5u << 19) : (i << 19));
	}
	if (fw_mode == 2) memset(ipc + off + 2 * size, 'a', 32);
	if (fw_mode == 3) strcpy((char *)ipc + off + 3 * size, "audio_playback0");
	if (fw_mode == 4) ipc[off + 4 * size + 3] = 0x01;
	wr32le(ipc, SWAS_AOC_MAGIC);
}

static int
f_r32(void *c, int w, uint32_t off, uint32_t *v)
{
	(void)c;
	if (w < 0 || w >= SWAS_NWIN || (off & 3u) || off > swas_win_len[w] - 4u) return 0;
	*v = win[w][off / 4];
	return 1;
}

static int
f_w32(void *c, int w, uint32_t off, uint32_t v)
{
	(void)c;
	if (w < 0 || w >= SWAS_NWIN || (off & 3u) || off > swas_win_len[w] - 4u) return 0;
	if (nw < MAXW) { wl[nw].w = w; wl[nw].off = off; wl[nw].v = v; }
	nw++;
	win[w][off / 4] = v;
	return 1;
}

static int f_imap(void *c, uint64_t pa, uint32_t len) { (void)c; CHECK(!mapped && len == SWAS_IPC_LEN, "imap"); mapped = 1; maps++; map_pa = pa; return 1; }
static void f_iunmap(void *c) { (void)c; mapped = 0; }
static int
f_ir32(void *c, uint32_t off, uint32_t *v)
{
	(void)c;
	CHECK(mapped, "ipc read while unmapped");
	if (!mapped || (off & 3u) || off > SWAS_IPC_LEN - 4u) return 0;
	*v = rd32le(ipc + off);
	if (corrupt_param && off == 8) *v ^= 1;
	return 1;
}
static int
f_iw32(void *c, uint32_t off, uint32_t v)
{
	(void)c;
	CHECK(mapped, "ipc write while unmapped");
	if (!mapped || (off & 3u) || off > SWAS_IPC_LEN - 4u) return 0;
	CHECK(off < 4u * SWAS_PARAM_WORDS, "ipc write outside the param block %#x", off);
	wr32le(ipc + off, v);
	return 1;
}
static void f_isync(void *c) { (void)c; synced++; }

static void
f_tipc(void *c, struct swas_tipc *t)
{
	uint32_t k;
	(void)c;
	tipc_calls++;
	synced_before_tipc = synced;
	param_ok_at_tipc = rd32le(ipc) == SWAS_PARAM_MAGIC && rd32le(ipc + 4) == 22 && rd32le(ipc + 8) == 276;
	ssmt_at_tipc = 1;
	for (k = 0; k < 64; k++) ssmt_at_tipc &= win[SWAS_W_SSMT][swas_ssmt_off(k) / 4] == SWAS_SSMT_BYPASS;
	*t = tscript;
}

static void
f_sleep(void *c, uint32_t ms)
{
	(void)c;
	now_ms += ms;
	slept += ms;
	if (fw_mode && tscript.start_replied && now_ms >= ready_at && rd32le(ipc) != SWAS_AOC_MAGIC) fw_write_table();
}
static void f_say(void *c, const char *w) { (void)c; (void)w; }
static uint32_t f_rand(void *c) { (void)c; return 0xC0FFEE11u; }

static const struct swas_bus bus = { f_r32, f_w32, f_imap, f_iunmap, f_ir32, f_iw32, f_isync, f_tipc, f_sleep, f_say, f_rand, NULL };
static struct swas_res r;

static void
reset(void)
{
	uint32_t k;
	memset(win, 0, sizeof win);
	memset(ipc, 0xA5, 0x4000);
	nw = mapped = maps = tipc_calls = synced = synced_before_tipc = param_ok_at_tipc = ssmt_at_tipc = corrupt_param = 0;
	now_ms = slept = 0;
	ready_at = 1200;
	fw_mode = 1;
	win[SWAS_W_CHIPID][0] = 0x09845011u;            /* GS101, main rev 1 */
	win[SWAS_W_CHIPID][0x10 / 4] = 0x00010000u;     /* sub rev 1 -> 0x11 */
	win[SWAS_W_REQ][0] = 1;
	win[SWAS_W_REQ][0x40 / 4] = 1;
	win[SWAS_W_MMU][0] = 0x24;
	for (k = 0; k < 64; k++) win[SWAS_W_SSMT][swas_ssmt_off(k) / 4] = k == 5 ? SWAS_SSMT_BYPASS : 0x1000u + k;
	tscript = (struct swas_tipc){ 0, "ok", 3, { 1, 2, 2 }, { 0, 0, 0 }, 1, 1 };
}

/* writes: the 64 SSMT bypass writes, [64 restores], [aoc_req := 0]; nothing else */
static void
check_writes(const char *name, int restored, int released)
{
	int i, exp = 64 + (restored ? 64 : 0) + (released ? 1 : 0);
	CHECK(nw == exp, "%s: %d register writes, expected %d", name, nw, exp);
	for (i = 0; i < 64 && i < nw; i++) {
		CHECK(wl[i].w == SWAS_W_SSMT && wl[i].off == swas_ssmt_off((uint32_t)i) && wl[i].v == SWAS_SSMT_BYPASS, "%s: write %d", name, i);
	}
	for (i = 64; restored && i < 128 && i < nw; i++) {
		uint32_t k = (uint32_t)i - 64u;
		CHECK(wl[i].w == SWAS_W_SSMT && wl[i].off == swas_ssmt_off(k) && wl[i].v == (k == 5 ? SWAS_SSMT_BYPASS : 0x1000u + k),
		    "%s: restore %d", name, i);
	}
	if (released && nw == exp) {
		CHECK(wl[nw - 1].w == SWAS_W_REQ && wl[nw - 1].off == 0 && wl[nw - 1].v == 0, "%s: last write aoc_req := 0", name);
	}
	CHECK(!mapped, "%s: ipc left mapped", name);
}

int
main(void)
{
	uint32_t k, sid[64];
	int rc;

	/* SSMT offsets: stream ids 0..30, 32 for read then write (aoc_v1.c:23-24, 331-336) */
	for (k = 0; k < 64; k++) sid[k] = swas_ssmt_off(k);
	CHECK(sid[0] == 0x4000 && sid[30] == 0x4078 && sid[31] == 0x4080 && sid[32] == 0x4200 && sid[63] == 0x4280, "ssmt offsets");

	/* 1. good start */
	reset();
	rc = swas_run(&bus, &r, 0x1400000u);
	CHECK(rc == SWAS_OK, "good: rc %d", rc);
	CHECK(map_pa == 0x95400000ull, "good: ipc pa %#llx", (unsigned long long)map_pa);
	CHECK(r.services == 5 && r.has_ctl && r.ctl_index == 0 && !strcmp(r.names[1], "audio_playback0") && r.flags[1] == 0x00290000u,
	    "good: services %u ctl %u", r.services, r.has_ctl);
	CHECK(!strcmp(r.fw_name, "aoc-fake-1") && r.fw_version == 0x1234, "good: fw name");
	CHECK(r.waited_ms >= 1200 && r.waited_ms <= 1210 && tipc_calls == 1, "good: waited %u", r.waited_ms);
	CHECK(param_ok_at_tipc && ssmt_at_tipc && synced_before_tipc >= 1, "good: param block + ssmt before START");
	CHECK(r.chip_rev == 0x11 && r.chip_type == 0x11 && r.chip_prod == 0x09845000u, "good: chip");
	/* the parameter block, aoc.c:503-526 with the oriole values */
	{
		static const uint32_t exp[22][2] = { { 0x1001, 0x20302 }, { 0x1002, 0x10000 }, { 0x1003, 0 }, { 0x1005, 0x94000000u },
			{ 0x1006, 0x3000000 }, { 0x1007, 0x9AC00000u }, { 0x1008, 0x400000 }, { 0x100C, 0x9ABFC000u }, { 0x100D, 0x4000 },
			{ 0x100E, 0x9ABEC000u }, { 0x100F, 0x10000 }, { 0x1009, 0 }, { 0x100A, 0 }, { 0x100B, 0 }, { 0x1010, 0 },
			{ 0x1011, 0xC0FFEE11u }, { 0x1012, 0x11 }, { 0x1013, 0x11 }, { 0x1014, 0 }, { 0x1015, 0 }, { 0x1016, 0x09845000u },
			{ 0x1017, 0 } };
		CHECK(r.param[0] == 0x0a0cda7a && r.param[1] == 22 && r.param[2] == 276, "param header");
		for (k = 0; k < 22; k++) {
			CHECK(r.param[3 + 3 * k] == exp[k][0] && r.param[4 + 3 * k] == 4 && r.param[5 + 3 * k] == exp[k][1],
			    "param %u: %#x %#x", k, r.param[3 + 3 * k], r.param[5 + 3 * k]);
		}
	}
	check_writes("good", 0, 1);

	/* 2. START answered with an error code: SSMT restored, no ready poll, no aoc_req write, dump */
	reset();
	tscript = (struct swas_tipc){ 13, "hwmgr-error", 2, { 1, 0, 0 }, { 0, 5, 0 }, 1, 1 };
	rc = swas_run(&bus, &r, 0x1400000u);
	CHECK(rc == SWAS_E_TIPC && r.dumped && slept == 0, "start err: rc %d", rc);
	check_writes("start err", 1, 0);

	/* 3. gate: GET_STATE says INACTIVE, START never sent */
	reset();
	tscript = (struct swas_tipc){ 15, "start-gate", 1, { 0, 0, 0 }, { 0, 0, 0 }, 0, 0 };
	rc = swas_run(&bus, &r, 0x1400000u);
	CHECK(rc == SWAS_E_TIPC, "gate: rc %d", rc);
	check_writes("gate", 1, 0);

	/* 4. START reply lost (timeout after send): the AOC may run, SSMT stays */
	reset();
	tscript = (struct swas_tipc){ 3, "timeout", 1, { 1, 0, 0 }, { 0, 0, 0 }, 1, 0 };
	rc = swas_run(&bus, &r, 0x1400000u);
	CHECK(rc == SWAS_E_TIPC && r.dumped, "start lost: rc %d", rc);
	check_writes("start lost", 0, 0);

	/* 5. undocumented state */
	reset();
	tscript.st[2] = 7;
	rc = swas_run(&bus, &r, 0x1400000u);
	CHECK(rc == SWAS_E_UNDOC && r.dumped, "undoc: rc %d", rc);
	check_writes("undoc", 0, 0);

	/* 5b. transport says ok but the first state was not LOADED (inconsistent peer): stop, no ready poll */
	reset();
	tscript.st[0] = 0;
	rc = swas_run(&bus, &r, 0x1400000u);
	CHECK(rc == SWAS_E_TIPC && slept == 0, "not loaded: rc %d", rc);
	check_writes("not loaded", 0, 0);

	/* 6. no ready within 5 s */
	reset();
	fw_mode = 0;
	win[SWAS_W_MBOX][0x4C / 4] = 0x1;
	rc = swas_run(&bus, &r, 0x1400000u);
	CHECK(rc == SWAS_E_NOREADY && r.dumped && r.d_intsr1 == 1 && r.d_req == 1 && r.d_mmu_ctrl == 0x24, "no ready: rc %d", rc);
	CHECK(slept >= SWAS_READY_MS && slept <= SWAS_READY_MS + SWAS_POLL_MS, "no ready: bounded %u ms", slept);
	check_writes("no ready", 0, 0);

	/* 7. malformed service tables */
	for (fw_mode = 2; fw_mode <= 6; fw_mode++) {
		int m = fw_mode;
		static const uint32_t why[7] = { 0, 0, 4, 6, 3, 1, 2 }, idx[7] = { 0, 0, 2, 3, 4, ~0u, ~0u };
		reset();
		fw_mode = m;
		rc = swas_run(&bus, &r, 0x1400000u);
		CHECK(rc == SWAS_E_SVC && r.bad_why == why[m] && r.bad_index == idx[m], "malformed %d: rc %d why %u idx %d", m, rc,
		    r.bad_why, (int)r.bad_index);
		check_writes("malformed", 0, 0);
		fw_mode = m;
	}

	/* 8. valid table without audio_output_control */
	reset();
	fw_mode = 7;
	rc = swas_run(&bus, &r, 0x1400000u);
	CHECK(rc == SWAS_E_NOCTL && r.services == 5 && !r.has_ctl, "no ctl: rc %d", rc);
	check_writes("no ctl", 0, 1);

	/* 9. stops before any write */
	reset();
	win[SWAS_W_CHIPID][0] = 0x09855011u;
	rc = swas_run(&bus, &r, 0x1400000u);
	CHECK(rc == SWAS_E_CHIP && nw == 0 && maps == 0 && tipc_calls == 0, "chip: rc %d", rc);
	reset();
	win[SWAS_W_REQ][0x40 / 4] = 0;
	rc = swas_run(&bus, &r, 0x1400000u);
	CHECK(rc == SWAS_E_REQ && nw == 0 && maps == 0 && tipc_calls == 0, "req: rc %d", rc);
	reset();
	rc = swas_run(&bus, &r, 0x1400123u);
	CHECK(rc == SWAS_E_IMAGE && nw == 0 && maps == 0, "image size: rc %d", rc);
	reset();
	rc = swas_run(&bus, &r, SWAS_HEAP_LOW);
	CHECK(rc == SWAS_E_IMAGE && nw == 0, "image size high: rc %d", rc);
	reset();
	corrupt_param = 1;
	rc = swas_run(&bus, &r, 0x1400000u);
	CHECK(rc == SWAS_E_PARAM && nw == 0 && tipc_calls == 0 && !mapped, "param: rc %d", rc);

	printf("aoc_start_host_test: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
