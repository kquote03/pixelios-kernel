/*
 * Host test of the Trusty TIPC GET_STATE step (xnu patch 0162, bsd/dev/sweet_tipc.h) against a fake Trusty that checks
 * the wire formats it receives (FF-A share descriptor, virtio status/features/vring handles, TIPC connect/data/disconnect)
 * the way the stock Linux driver builds them (docs/research/50). Also: the allowlist, bounds, and teardown on every stop.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "sweet_tipc.h"

#define PA0     0x880000000ull
#define MAXC    8000
static uint8_t arena[SWTP_ARENA] __attribute__((aligned(4096)));

struct share { uint64_t h, pa; uint32_t pages, perm, live; };
struct fake {
	/* injections */
	uint32_t api, ffa_ver, getdescr_ret_neg, num_vdev, dfeat, conn_status, herr, state, never_online, share_fail_at;
	uint32_t intr_descr, fiq_start, busy_reset, nopint, msgmax, cpu_idle_start, reclaim_deny, reset_idle_fail, reply_src, reply_dst;
	/* state */
	uint64_t now;
	uint32_t mapped, unmapped, nsh, started, stopped, resets, online_sent, conn, got_req, got_disc, errors, pending;
	uint32_t status_at_start, gfeat_at_start, tx_seen, rx_used, descr_len;
	struct share sh[32];
	uint64_t descr_h;
	uint32_t nc;
	uint64_t log[MAXC][4];
	char why[160];
};
static struct fake F;

#define ERR(...) do { if (!F.errors++) snprintf(F.why, sizeof F.why, __VA_ARGS__); } while (0)

static uint8_t *
pa2p(uint64_t pa, uint32_t len)
{
	if (pa < PA0 || pa + len > PA0 + SWTP_ARENA) {
		ERR("pa %#llx out of arena", (unsigned long long)pa);
		return NULL;
	}
	return arena + (pa - PA0);
}

static struct share *
byh(uint64_t h)
{
	for (uint32_t i = 0; i < F.nsh; i++) {
		if (F.sh[i].h == h && F.sh[i].live) {
			return &F.sh[i];
		}
	}
	return NULL;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

/* vring access through the handles written into the descriptor */
static uint8_t *dsc(void) { struct share *s = byh(F.descr_h); return s ? pa2p(s->pa, 4096) : NULL; }
static uint8_t *vdev(void) { uint8_t *d = dsc(); return d ? d + rd32(d + 16) + 4 : NULL; }
static uint8_t *
vring(int q, struct share **so)
{
	uint8_t *v = vdev(), *e;
	struct share *s;

	if (!v) {
		return NULL;
	}
	e = v + 24 + 20 * q;
	s = byh((uint64_t)rd32(e) | (uint64_t)rd32(e + 16) << 32);
	if (!s) {
		ERR("vring %d handle unknown", q);
		return NULL;
	}
	if (so) {
		*so = s;
	}
	return pa2p(s->pa, s->pages * 4096);
}

static uint32_t uoff(void) { return (16u * 32 + 2u * (3 + 32) + 4095) & ~4095u; }

static void
rx_put(uint32_t src, uint32_t dst, const uint32_t *w, uint32_t nw)
{
	uint8_t *v = vring(0, NULL), *av, *u, *b;
	uint16_t aidx, d;
	struct share *s;

	if (!v) {
		return;
	}
	av = v + 16 * 32;
	u = v + uoff();
	aidx = rd16(av + 2);
	if (aidx == (uint16_t)F.rx_used) {
		ERR("no rx buffer posted");
		return;
	}
	d = rd16(av + 4 + 2 * (F.rx_used % 32));
	s = byh(rd64(v + 16 * d));
	if (!s || s->perm != SWTP_FFA_RW || rd32(v + 16 * d + 8) != 4096 || rd16(v + 16 * d + 12) != SWTP_DESC_F_WRITE) {
		ERR("rx desc %u bad", d);
		return;
	}
	b = pa2p(s->pa, 4096);
	memset(b, 0, 4096);
	wr32(b, src);
	wr32(b + 4, dst);
	wr16(b + 12, (uint16_t)(4 * nw));
	for (uint32_t i = 0; i < nw; i++) {
		wr32(b + 16 + 4 * i, w[i]);
	}
	wr32(u + 4 + 8 * (F.rx_used % 32), d);
	wr32(u + 8 + 8 * (F.rx_used % 32), 16 + 4 * nw);
	F.rx_used++;
	wr16(u + 2, (uint16_t)F.rx_used);
}

static void
tx_process(void)
{
	uint8_t *v = vring(1, NULL), *av, *b;
	uint16_t aidx, d;
	struct share *s;
	uint32_t len, plen, src, dst;

	if (!v || !F.started) {
		return;
	}
	av = v + 16 * 32;
	aidx = rd16(av + 2);
	while ((uint16_t)F.tx_seen != aidx) {
		d = rd16(av + 4 + 2 * (F.tx_seen % 32));
		F.tx_seen++;
		s = byh(rd64(v + 16 * d));
		len = rd32(v + 16 * d + 8);
		if (!s || s->perm != SWTP_FFA_RO || rd16(v + 16 * d + 12) != 0) {
			ERR("tx desc %u bad", d);
			return;
		}
		b = pa2p(s->pa, 4096);
		src = rd32(b);
		dst = rd32(b + 4);
		plen = rd16(b + 12);
		if (src != 1024 || plen + 16 != len || rd16(b + 10) != 0 || rd16(b + 14) != 0 || rd16(b + 8) != 0) {
			ERR("tx hdr bad src %u plen %u len %u", src, plen, len);
			return;
		}
		if (dst == 53 && rd32(b + 16) == 3) {
			if (plen != 264 || rd32(b + 20) != 256 || strcmp((char *)b + 24, "com.android.trusty.gsa.hwmgr.aoc") || b[24 + 255]) {
				ERR("connect msg bad");
				return;
			}
			F.conn++;
			uint32_t w[7] = { 4, 20, 1024, F.conn_status, 5000, 4096, 32 };
			rx_put(53, 53, w, 7);
		} else if (dst == 5000 && plen == 8) {
			if (rd32(b + 16) != 1 || rd32(b + 20) != 0) {
				ERR("hwmgr req bad %u %u", rd32(b + 16), rd32(b + 20));
				return;
			}
			F.got_req++;
			uint32_t w[3] = { 0x80000001u, F.herr, F.state };
			rx_put(F.reply_src ? F.reply_src : 5000, F.reply_dst ? F.reply_dst : 1024, w, 3);
		} else if (dst == 53 && rd32(b + 16) == 5) {
			if (plen != 12 || rd32(b + 20) != 4 || rd32(b + 24) != 5000) {
				ERR("disc bad");
			}
			F.got_disc++;
		} else {
			ERR("unexpected tx dst %u", dst);
		}
	}
}

static void
fsmc(void *c, uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3, struct swtp_ret *o)
{
	(void)c;
	F.now += 5;
	if (F.nc < MAXC) {
		F.log[F.nc][0] = fid; F.log[F.nc][1] = a1; F.log[F.nc][2] = a2; F.log[F.nc][3] = a3;
	}
	F.nc++;
	o->r0 = o->r1 = o->r2 = o->r3 = 0;
	switch (fid) {
	case SWTP_FC_API_VERSION: o->r0 = F.api; return;
	case SWTP_FFA_VERSION: o->r0 = F.ffa_ver; return;
	case SWTP_FFA_FEATURES: o->r0 = SWTP_FFA_SUCCESS; return;
	case SWTP_FFA_ID_GET: o->r0 = SWTP_FFA_SUCCESS; o->r2 = 0x0001; return;
	case SWTP_FFA_RXTX_MAP64:
		if (a1 != PA0 + SWTP_OFF_FTX || a2 != PA0 + SWTP_OFF_FRX || a3 != 1) ERR("rxtx args");
		F.mapped = 1; o->r0 = SWTP_FFA_SUCCESS; return;
	case SWTP_FFA_RXTX_UNMAP: F.unmapped++; o->r0 = SWTP_FFA_SUCCESS; return;
	case SWTP_FFA_MEM_SHARE: {
		uint8_t *t = arena + SWTP_OFF_FTX;
		if (!F.mapped) ERR("share before map");
		if (F.share_fail_at && F.nsh + 1 == F.share_fail_at) { o->r0 = 0x84000060u; o->r2 = (uint32_t)-3; return; }
		if (rd16(t) != 1 || t[2] != 0x2F || t[3] || rd32(t + 4) || rd64(t + 8) || rd64(t + 16) || rd32(t + 24) ||
		    rd32(t + 28) != 1 || rd16(t + 32) != 0x8000 || (t[34] != 1 && t[34] != 2) || t[35] || rd32(t + 36) != 48 ||
		    rd64(t + 40) || rd32(t + 52) != 1 || rd64(t + 56) || rd32(t + 48) != rd32(t + 72) || rd32(t + 76) ||
		    (rd64(t + 64) & 4095) || a1 != 80 || a2 != 80)
			ERR("share descriptor bad");
		struct share *s = &F.sh[F.nsh];
		s->h = 0x100000000ull + 0xC3 + F.nsh;
		s->pa = rd64(t + 64); s->pages = rd32(t + 72); s->perm = t[34]; s->live = 1;
		pa2p(s->pa, s->pages * 4096);
		F.nsh++;
		o->r0 = SWTP_FFA_SUCCESS; o->r2 = (uint32_t)s->h; o->r3 = s->h >> 32; return;
	}
	case SWTP_FFA_MEM_RECLAIM: {
		struct share *s = byh(a1 | a2 << 32);
		if (!s) { ERR("reclaim unknown"); o->r0 = 0x84000060u; return; }
		if (F.reclaim_deny) { o->r0 = 0x84000060u; o->r2 = (uint32_t)-6; return; }
		s->live = 0; o->r0 = SWTP_FFA_SUCCESS; return;
	}
	case SWTP_SC_RESTART_FIQ: o->r0 = F.pending; F.pending = 0; return;
	case SWTP_SC_RESTART_LAST: o->r0 = F.pending; F.pending = 0; return;
	case SWTP_SC_GET_DESCR: {
		struct share *s = byh(a1 | a2 << 32);
		uint8_t *d;
		if (!s || s->perm != 2 || a3 != 4096) { ERR("get_descr args"); o->r0 = (uint32_t)-2; return; }
		F.descr_h = s->h;
		if (F.getdescr_ret_neg) { o->r0 = (uint32_t)-7; return; }
		d = pa2p(s->pa, 4096);
		memset(d, 0, 4096);
		wr32(d, 1); wr32(d + 4, F.num_vdev); wr32(d + 16, 20);
		uint8_t *v = d + 24;
		wr32(d + 20, 3);                /* entry at 20: type RSC_VDEV */
		wr32(v, 13); wr32(v + 4, 9); wr32(v + 8, F.dfeat); wr32(v + 16, 40); v[20] = 0; v[21] = 2;
		for (int i = 0; i < 2; i++) { uint8_t *e = v + 24 + 20 * i; wr32(e + 4, 4096); wr32(e + 8, 32); wr32(e + 12, 1 + i); }
		wr32(v + 64, F.msgmax); wr32(v + 68, 4096); memcpy(v + 72, "dev0", 4);
		F.descr_len = 24 + 24 + 40 + 40;
		if (F.intr_descr) { F.intr_descr = 0; F.pending = F.descr_len; o->r0 = SWTP_E_INTERRUPTED; return; }
		o->r0 = F.descr_len; return;
	}
	case SWTP_SC_VDEV_RESET:
		if (a1 != 9) ERR("reset nid");
		if (F.busy_reset) { F.busy_reset--; o->r0 = SWTP_E_BUSY; return; }
		/* measured o6-a5-tipc 20261117T093122Z: the probe reset before START returns SM_ERR_INTERNAL_FAILURE (-7) */
		if (F.reset_idle_fail && !F.started) { o->r0 = (uint32_t)-7; return; }
		F.resets++; o->r0 = 0; return;
	case SWTP_SC_START: {
		uint8_t *v = vdev();
		struct share *s0 = NULL, *s1 = NULL;
		if (F.fiq_start) { F.fiq_start = 0; F.pending = 0; o->r0 = SWTP_E_FIQ_INTERRUPTED; }
		if (F.cpu_idle_start) { F.cpu_idle_start = 0; F.pending = 0; o->r0 = SWTP_E_CPU_IDLE; }
		if (!v || a3 != F.descr_len || (a1 | a2 << 32) != F.descr_h) { ERR("start args"); o->r0 = (uint32_t)-2; return; }
		F.status_at_start = v[20];
		F.gfeat_at_start = rd32(v + 12);
		if (!vring(0, &s0) || !vring(1, &s1) || s0->pages != 2 || s1->pages != 2 || s0->perm != 2 || s1->perm != 2)
			ERR("vrings at start");
		F.started = 1;
		if (!F.never_online) {
			uint32_t w[2] = { 1, 0 };
			rx_put(53, 53, w, 2);
		}
		if (o->r0 == 0) o->r0 = 0;
		return;
	}
	case SWTP_SC_NOP:
		if (F.nopint) { F.nopint--; o->r0 = SWTP_E_NOP_INTERRUPTED; return; }
		tx_process();
		o->r0 = SWTP_E_NOP_DONE; return;
	case SWTP_SC_STOP:
		if ((a1 | a2 << 32) != F.descr_h || a3 != 4096) ERR("stop args");
		F.stopped++; F.started = 0; o->r0 = 0; return;
	default:
		ERR("unknown fid %#llx", (unsigned long long)fid); o->r0 = 0xFFFFFFFFu; return;
	}
}

static uint64_t fnow(void *c) { (void)c; return F.now; }
static void fdelay(void *c, uint32_t us) { (void)c; F.now += us; }

static void
reset(void)
{
	memset(&F, 0, sizeof F);
	F.api = 5; F.ffa_ver = 0x10000; F.num_vdev = 1; F.msgmax = 4096; F.now = 1000;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf(" (fake: %s)\n", F.why); fails++; } } while (0)

static uint32_t
live(void)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < F.nsh; i++) n += F.sh[i].live;
	return n;
}

/* every call the code issued is on the allowlist; non-repeatable calls never repeat with other arguments */
static void
check_log(const char *name)
{
	for (uint32_t i = 0; i < F.nc && i < MAXC; i++) {
		uint64_t f = F.log[i][0];
		int ok = f == SWTP_FC_API_VERSION || f == SWTP_FFA_VERSION || f == SWTP_FFA_FEATURES || f == SWTP_FFA_ID_GET ||
		    f == SWTP_FFA_RXTX_MAP64 || f == SWTP_FFA_RXTX_UNMAP || f == SWTP_FFA_MEM_SHARE || f == SWTP_FFA_MEM_RECLAIM ||
		    f == SWTP_SC_GET_DESCR || f == SWTP_SC_START || f == SWTP_SC_STOP || f == SWTP_SC_VDEV_RESET ||
		    f == SWTP_SC_NOP || f == SWTP_SC_RESTART_LAST || f == SWTP_SC_RESTART_FIQ;
		CHECK(ok, "%s: fid %#llx not allowlisted", name, (unsigned long long)f);
		if (f == SWTP_SC_NOP) {
			CHECK((!F.log[i][1] && !F.log[i][2] && !F.log[i][3]) ||
			    (F.log[i][1] == SWTP_NC_KICK_VQ && F.log[i][2] == 9 && (F.log[i][3] == 1 || F.log[i][3] == 2)),
			    "%s: NOP args", name);
		}
		if (f == SWTP_FFA_MEM_SHARE || f == SWTP_FFA_MEM_RECLAIM || f == SWTP_SC_NOP || f == SWTP_SC_RESTART_LAST ||
		    f == SWTP_SC_RESTART_FIQ) {
			continue;
		}
		for (uint32_t j = 0; j < i; j++) {
			if (F.log[j][0] == f) {
				CHECK(F.log[j][1] == F.log[i][1] && F.log[j][2] == F.log[i][2] && F.log[j][3] == F.log[i][3],
				    "%s: fid %#llx repeated with other arguments", name, (unsigned long long)f);
			}
		}
		if (f == SWTP_FFA_MEM_RECLAIM) {
			continue;
		}
	}
	/* a reclaim never names the same handle twice */
	for (uint32_t i = 0; i < F.nc && i < MAXC; i++) {
		for (uint32_t j = 0; j < i; j++) {
			if (F.log[i][0] == SWTP_FFA_MEM_RECLAIM && F.log[j][0] == SWTP_FFA_MEM_RECLAIM) {
				CHECK(F.log[i][1] != F.log[j][1] || F.log[i][2] != F.log[j][2], "%s: reclaim twice", name);
			}
		}
	}
}

static int
run(struct swtp_res *r)
{
	struct swtp_bus b = { fsmc, fnow, fdelay, NULL, arena, PA0 };
	uint64_t t0 = F.now;
	int rc = swtp_run(&b, r);

	CHECK(F.now - t0 <= SWTP_RUN_US + 3u * SWTP_CALL_US, "bounded total time %llu", (unsigned long long)(F.now - t0));
	return rc;
}

int
main(void)
{
	static struct swtp_res r;
	int rc;

	reset();
	rc = run(&r);
	CHECK(rc == SWTP_OK && r.state == 0 && F.errors == 0, "happy: rc %s state %u", swtp_ename(rc), r.state);
	CHECK(F.status_at_start == 7 && F.gfeat_at_start == 0, "happy: status %u gfeat %#x at START", F.status_at_start, F.gfeat_at_start);
	CHECK(F.resets == 2 && F.stopped == 1 && F.unmapped == 1 && live() == 0 && r.td_bad == 0, "happy: teardown resets %u stop %u unmap %u live %u",
	    F.resets, F.stopped, F.unmapped, live());
	CHECK(F.nsh == 10 && r.nreclaim == 10 && F.conn == 1 && F.got_req == 1 && F.got_disc == 1, "happy: shares %u conn %u req %u disc %u",
	    F.nsh, F.conn, F.got_req, F.got_disc);
	CHECK(r.vr_size[0] == 8192 && r.remote == 5000 && r.max_msg == 4096, "happy: decode");
	check_log("happy");

	reset(); F.state = 1; F.intr_descr = 1; F.fiq_start = 1; F.busy_reset = 2; F.nopint = 3; F.dfeat = 0x30000001u;
	rc = run(&r);
	CHECK(rc == SWTP_OK && r.state == 1 && F.errors == 0, "restarts: rc %s", swtp_ename(rc));
	CHECK(F.gfeat_at_start == 0x30000001u, "restarts: gfeat %#x", F.gfeat_at_start);
	check_log("restarts");

	reset(); F.cpu_idle_start = 1;
	rc = run(&r);
	CHECK(rc == SWTP_OK && F.errors == 0, "cpu idle: rc %s", swtp_ename(rc));

	reset(); F.api = 4;
	rc = run(&r);
	CHECK(rc == SWTP_E_API && F.nc == 1, "api 4: rc %s calls %u", swtp_ename(rc), F.nc);

	reset(); F.ffa_ver = 0x20000;
	rc = run(&r);
	CHECK(rc == SWTP_E_FFA && !F.mapped && F.nsh == 0, "ffa ver: rc %s", swtp_ename(rc));

	reset(); F.getdescr_ret_neg = 1;
	rc = run(&r);
	CHECK(rc == SWTP_E_DESCR && F.stopped == 0 && live() == 0 && F.unmapped == 1, "get_descr <0: rc %s stop %u live %u", swtp_ename(rc), F.stopped, live());
	check_log("getdescr");

	reset(); F.num_vdev = 2;
	rc = run(&r);
	CHECK(rc == SWTP_E_DESCR && F.stopped == 1 && F.resets == 0 && live() == 0, "two vdevs: rc %s stop %u", swtp_ename(rc), F.stopped);

	reset(); F.msgmax = 8192;
	rc = run(&r);
	CHECK(rc == SWTP_E_DESCR && F.stopped == 1 && live() == 0, "msg max 8192: rc %s", swtp_ename(rc));

	reset(); F.never_online = 1;
	rc = run(&r);
	CHECK(rc == SWTP_E_ONLINE && F.stopped == 1 && F.resets == 2 && live() == 0 && F.conn == 0, "never online: rc %s", swtp_ename(rc));
	check_log("never-online");

	reset(); F.conn_status = 1;
	rc = run(&r);
	CHECK(rc == SWTP_E_CONN && F.got_req == 0 && F.got_disc == 0 && live() == 0, "refused: rc %s", swtp_ename(rc));

	reset(); F.herr = 3; F.state = 0;
	rc = run(&r);
	CHECK(rc == SWTP_E_HWMGR && r.herr == 3 && F.got_disc == 1 && live() == 0, "hwmgr err: rc %s", swtp_ename(rc));
	check_log("hwmgr-err");

	reset(); F.share_fail_at = 5;
	rc = run(&r);
	CHECK(rc == SWTP_E_SHARE && F.started == 0 && live() == 0 && F.stopped == 1 && F.resets == 2, "share fail: rc %s", swtp_ename(rc));

	reset(); F.reclaim_deny = 1;
	rc = run(&r);
	CHECK(rc == SWTP_OK && r.td_bad == 10 && r.nreclaim == 0, "reclaim denied: rc %s td_bad %u", swtp_ename(rc), r.td_bad);
	check_log("reclaim-deny");

	reset(); F.busy_reset = 1000;     /* BUSY forever: each reset at most 6 times, result ignored as Linux does */
	rc = run(&r);
	CHECK(rc == SWTP_OK && r.reset_r0 == SWTP_E_BUSY && (r.td_mask & 1u) && F.nc < 200, "busy forever: rc %s calls %u", swtp_ename(rc), F.nc);

	reset(); F.reset_idle_fail = 1;   /* hardware: probe VDEV_RESET -7 must not stop the probe (trusty-virtio.c:211-219) */
	rc = run(&r);
	CHECK(rc == SWTP_OK && r.reset_r0 == (uint32_t)-7 && F.resets == 1 && F.stopped == 1 && r.td_bad == 0 && live() == 0 && F.errors == 0,
	    "reset -7 before start: rc %s stage %d reset_r0 %#x resets %u td_bad %u", swtp_ename(rc), r.stage, r.reset_r0, F.resets, r.td_bad);
	check_log("reset-idle-fail");

	reset(); F.reply_src = 1001;   /* o6-a5-tipc2: reply src != CONN_RSP remote; Linux routes by dst only (trusty-ipc.c:2071-2080) */
	rc = run(&r);
	CHECK(rc == SWTP_OK && r.rsrc == 1001 && r.rdst == 1024 && F.got_disc == 1 && live() == 0 && F.errors == 0, "reply src: rc %s", swtp_ename(rc));

	reset(); F.reply_dst = 1025;   /* not our channel: Linux drops it, we stop */
	rc = run(&r);
	CHECK(rc == SWTP_E_REPLY && r.rdst == 1025 && F.got_disc == 1 && live() == 0, "reply dst: rc %s", swtp_ename(rc));

	reset(); F.nopint = 100000;     /* a Trusty that never finishes a NOP: every wait still ends */
	rc = run(&r);
	CHECK(rc != SWTP_OK, "nop forever: rc %s", swtp_ename(rc));

	{
		struct swtp_bus bad = { fsmc, fnow, fdelay, NULL, arena + 8, PA0 };
		reset();
		CHECK(swtp_run(&bad, &r) == SWTP_E_ARG && F.nc == 0, "misaligned arena refused");
	}
	printf("RESULT: %s tipc_host_test (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
