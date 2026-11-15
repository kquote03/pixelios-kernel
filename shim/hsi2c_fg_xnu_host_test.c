/* Host replay of the XNU fuel-gauge reader of xnu patch 0148 (bsd/dev/sweet_hsi2c_fg.h, extracted from the patch by build.sh) against a FAKE
 * HSI2C12 controller + fake MAX77759 fuel gauge, modelled on shim/fgread_host_test.c of branch wip/shim-fg-read (the reviewed shim --fg-read).
 * The config is loaded through the header's own loader from the exact /sweet-batt property bytes tools/mkadt.py puts in the ADT
 * (out/sweet_batt_adt.h, written by build.sh from mkadt.sweet_batt_c("oriole")). Every offset, bit and value of the fake is typed here from the
 * driver text (Google soc-gs / Linux i2c-exynos5.c), never taken from the header. The handoff state is the one observed in runs oriole-peri1 /
 * oriole-fg1, and the happy path replays the raw words of run oriole-fg1.
 * Checked: the exact write sequence and values (7 x 17 + 6 restore writes per set, the shim's order), MASTER_RUN only while ADDR holds 0x36 << 10
 * and only as pointer-write-then-repeated-start-read, only the seven pointer bytes ever sent, no other register written or read, every write
 * inside one crit section; every refusal performs ZERO writes; NACK / timeout / arbitration / ERR_STATUS / faulting write / dropped writes all
 * restore and stop; a failed restore is loud; the stop latch performs no access afterwards; the writer allowlist (every other offset refused
 * with zero bus accesses, every other value refused, every other device address refused); the config loader refuses bad ADT values.
 * Build (build.sh, also with -fsanitize=address,undefined): gcc -O1 -Wall -Wextra -Werror -Iout -o out/hsi2c_fg_xnu_host_test hsi2c_fg_xnu_host_test.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sweet_hsi2c_fg.h"
#include "sweet_batt_adt.h"

/* ---- the independent constants (typed from the sources, never from the header) ---- */
#define R_CTL		0x00u	/* SGS i2c-exynos5.c :46-69 register map */
#define R_FIFO_CTL	0x04u
#define R_TRAIL		0x08u
#define R_INT_EN	0x20u
#define R_INT_ST	0x24u
#define R_ERR		0x2Cu
#define R_FIFO_ST	0x30u
#define R_TX		0x34u
#define R_RX		0x38u
#define R_CONF		0x40u
#define R_AUTO		0x44u
#define R_TMO		0x48u
#define R_TRANS		0x50u
#define R_ADDR		0x70u
#define R_USICON	0xC4u
#define CMU_IPCLK	0x2030u	/* CMU_PERIC1 offsets (soc.json notes.peri) */
#define CMU_PCLK	0x2050u
#define CMU_QCH		0x3030u
#define PAD_CON		0x0u	/* gpp23 bank CON / DAT */
#define PAD_DAT		0x4u

#define DONE		0x80u		/* INT_STATUS TRANS_DONE bit 7 */
#define ABORT		0x100u		/* bit 8 */
#define NODEVACK	0x200u		/* bit 9 */
#define RUN		0x80000000u	/* AUTO_CONF MASTER_RUN */
#define RW		0x10000u	/* AUTO_CONF READ_WRITE */
#define STOP		0x20000u	/* AUTO_CONF STOP_AFTER_TRANS */
#define MBUSY		0x20000u	/* TRANS_STATUS MASTER_BUSY bit 17 */

static const uint8_t SEVEN[7] = { 0x06, 0x08, 0x09, 0x0A, 0x0B, 0x35, 0x21 };	/* RepSOC Temp VCell Current AvgCurrent FullCapRep DevName */

/* ---- the fake controller ---- */
static struct {
	uint32_t ctl, fifo, trail, inten, ist, err, conf, aconf, tmo, trans, addr, usicon;
	uint32_t mode, ipclk, pclk, qch, pcon, pdat;
	uint8_t rx[16]; unsigned nrx;
	int held;
	int run_sticks;
	unsigned ptr; int have_ptr;
} h;
static uint16_t fgv[256];
static int fgv_known[256];
static struct { uint32_t off, v; } wlog[512];
static unsigned nw, nruns, nreads, violations, crit_depth, crit_n;
static char why_v[256];
static int rfault_sp = -1;
static uint32_t rfault_off;
static unsigned wfault_n, ignore_from;
static uint32_t ignore_off;
static int ignore_any;
static unsigned nack_at, timeout_at, rxtimeout_at, abort_at, err_at;
static int ist_sticky, busy_after_read;
static uint64_t ticks, tick_step;
static char lines[512][SFG_LINE + 8];
static unsigned nl;

static void violate(const char *s)
{
	if (!violations)
		snprintf(why_v, sizeof(why_v), "%s", s);
	violations++;
}

static uint32_t fifo_status(void)
{
	return ((uint32_t)h.nrx << 16) | (h.nrx ? 0u : (1u << 24)) | (1u << 8);
}

static int f_rd32(void *ctx, int sp, uint32_t off, uint32_t *v)
{
	(void)ctx;
	nreads++;
	if (sp == rfault_sp && off == rfault_off)
		return 0;
	if (sp == SFG_SP_MODE) { if (off) violate("mode word +off"); *v = h.mode; return 1; }
	if (sp == SFG_SP_CMU) {
		if (off == CMU_IPCLK) { *v = h.ipclk; return 1; }
		if (off == CMU_PCLK) { *v = h.pclk; return 1; }
		if (off == CMU_QCH) { *v = h.qch; return 1; }
		violate("CMU word outside the gate");
		return 0;
	}
	if (sp == SFG_SP_PAD) {
		if (off == PAD_CON) { *v = h.pcon; return 1; }
		if (off == PAD_DAT) { *v = h.pdat; return 1; }
		violate("pad word outside CON/DAT");
		return 0;
	}
	if (sp != SFG_SP_I2C) { violate("unknown space"); return 0; }
	switch (off) {
	case R_CTL: *v = h.ctl; return 1;
	case R_FIFO_CTL: *v = h.fifo; return 1;
	case R_TRAIL: *v = h.trail; return 1;
	case R_INT_EN: *v = h.inten; return 1;
	case R_INT_ST: *v = h.ist; return 1;
	case R_ERR: *v = h.err; return 1;
	case R_FIFO_ST: *v = fifo_status(); return 1;
	case R_CONF: *v = h.conf; return 1;
	case R_AUTO: *v = h.aconf; return 1;
	case R_TMO: *v = h.tmo; return 1;
	case R_TRANS:
		*v = h.trans;
		if (busy_after_read > 0 && (h.trans & MBUSY) && h.nrx == 0 && !h.held && --busy_after_read == 0)
			h.trans &= ~MBUSY;
		return 1;
	case R_ADDR: *v = h.addr; return 1;
	case R_USICON: *v = h.usicon; return 1;
	case R_RX:
		if (!h.nrx) { violate("RX_DATA read with the RX FIFO empty"); *v = 0; return 1; }
		*v = h.rx[0];
		memmove(h.rx, h.rx + 1, --h.nrx);
		return 1;
	case R_TX:
		violate("TX_DATA read");
		return 0;
	}
	violate("read outside the listed words");
	return 0;
}

static void start(uint32_t a)
{
	unsigned len = a & 0xffffu;

	nruns++;
	if (h.addr != 0x0000D800u)
		violate("MASTER_RUN while ADDR does not hold exactly 0x36 << 10");
	if (len == 0 || len > 2)
		violate("a transfer length other than 1 (pointer) or 2 (data)");
	if (nack_at == nruns) {
		h.ist |= NODEVACK;
		h.trans = (h.trans & ~0xfu) | 0xBu;
		h.held = 0;
		return;
	}
	if (abort_at == nruns) { h.ist |= ABORT; h.trans = (h.trans & ~0xfu) | 0xCu; h.held = 0; return; }
	if (err_at == nruns) { h.err = 0x1; return; }
	if (!(a & RW)) {
		if (len != 1)
			violate("a write message with data bytes");
		if (h.held)
			violate("a second write message while the bus is held");
		if ((h.ctl & 0xC0u) != 0x80u)
			violate("write message without TXCHON only");
		if (a & STOP)
			violate("the pointer write must not STOP");
		h.trans |= MBUSY;
		return;
	}
	if (!h.held)
		violate("a read message without the held pointer write (no repeated start)");
	if ((h.ctl & 0xC0u) != 0x40u)
		violate("read message without RXCHON only");
	if (!(a & STOP))
		violate("the data read must STOP");
	if (len != 2)
		violate("the data read must be 2 bytes");
	h.held = 0;
	if (rxtimeout_at == nruns)
		return;
	if (!h.have_ptr || !fgv_known[h.ptr])
		violate("read of a register the fake gauge does not have");
	h.rx[h.nrx++] = (uint8_t)(fgv[h.ptr] & 0xff);
	h.rx[h.nrx++] = (uint8_t)(fgv[h.ptr] >> 8);
	h.ist |= DONE;
	if (!busy_after_read)
		h.trans &= ~MBUSY;
	h.trans = (h.trans & ~0xfu) | 0x1u;
	if (!h.run_sticks)
		h.aconf &= ~RUN;
}

static int f_wr32(void *ctx, uint32_t off, uint32_t v)
{
	(void)ctx;
	if (nw >= 512) { violate("too many writes"); return 0; }
	if (crit_depth != 1)
		violate("a write outside one crit section");
	wlog[nw].off = off;
	wlog[nw].v = v;
	nw++;
	if (wfault_n && nw == wfault_n)
		return 0;
	if (ignore_from && nw >= ignore_from && (ignore_any || ignore_off == off))
		return 1;
	switch (off) {
	case R_CTL: h.ctl = v; return 1;
	case R_FIFO_CTL: h.fifo = v; return 1;
	case R_INT_EN: h.inten = v; return 1;
	case R_ADDR: h.addr = v; return 1;
	case R_INT_ST: if (!ist_sticky) h.ist &= ~v; return 1;
	case R_AUTO:
		h.aconf = v;
		if (v & RUN)
			start(v);
		return 1;
	case R_TX: {
		int ok = 0;

		for (unsigned i = 0; i < 7; i++)
			ok |= v == SEVEN[i];
		if (!ok)
			violate("a register pointer outside the seven");
		if (!(h.aconf & RUN) && !h.run_sticks)
			violate("TX byte with no write message running");
		if (h.nrx || (h.aconf & RW))
			violate("TX byte during a read");
		if (timeout_at == nruns)
			return 1;
		h.ptr = v & 0xff;
		h.have_ptr = 1;
		h.ist |= DONE;
		h.held = 1;
		if (!h.run_sticks)
			h.aconf &= ~RUN;
		return 1;
	}
	}
	violate("write to a register outside the seven of the allowlist");
	return 1;
}

static uint64_t f_now(void *ctx) { (void)ctx; ticks += tick_step; return ticks; }
static void f_crit(void *ctx, int enter)
{
	(void)ctx;
	if (enter) {
		if (crit_depth)
			violate("nested crit");
		crit_depth++;
		crit_n++;
	} else {
		if (crit_depth != 1)
			violate("crit leave without enter");
		crit_depth--;
	}
}
static void f_say(void *ctx, const char *line)
{
	(void)ctx;
	if (nl >= 512 || strlen(line) >= SFG_LINE) { violate("too many / too long lines"); return; }
	snprintf(lines[nl++], sizeof(lines[0]), "%s", line);
}

static struct sfg_ops ops;

/* ---- the ADT: the mkadt bytes, with an optional override for the loader tests ---- */
static const char *ov_name;
static uint32_t ov_val;
static int ov_drop;

static int p_u32(void *ctx, const char *name, uint32_t *v, unsigned n)
{
	(void)ctx;
	if (ov_name && !strcmp(name, ov_name)) {
		if (ov_drop)
			return 0;
		if (n == 1) { *v = ov_val; return 1; }
	}
	for (unsigned i = 0; i < sizeof(sfg_adt) / sizeof(sfg_adt[0]); i++)
		if (!strcmp(sfg_adt[i].name, name)) {
			if (sfg_adt[i].len != 4u * n)
				return 0;
			memcpy(v, sfg_adt[i].b, 4u * n);	/* little-endian host = the ADT byte order */
			if (ov_name && !strcmp(name, ov_name) && n > 1)
				v[0] = ov_val;
			return 1;
		}
	return 0;
}

static int p_pa(void *ctx, const char *name, uint64_t *pa, uint64_t *len)
{
	(void)ctx;
	if (ov_name && !strcmp(name, ov_name) && ov_drop)
		return 0;
	for (unsigned i = 0; i < sizeof(sfg_adt) / sizeof(sfg_adt[0]); i++)
		if (!strcmp(sfg_adt[i].name, name)) {
			if (sfg_adt[i].len != 16)
				return 0;
			memcpy(pa, sfg_adt[i].b, 8);
			memcpy(len, sfg_adt[i].b + 8, 8);
			return 1;
		}
	return 0;
}

static struct sfg_cfg cfg;
static struct sfg_state st;

static const char *load(void)
{
	struct sfg_props p = { p_u32, p_pa, NULL };

	memset(&cfg, 0, sizeof(cfg));
	return sfg_cfg_load(&cfg, &p);
}

/* the observed handoff state (runs oriole-peri1 / oriole-fg1) and the raw words of run oriole-fg1 */
static void world(void)
{
	memset(&h, 0, sizeof(h));
	memset(wlog, 0, sizeof(wlog));
	nw = nl = nruns = violations = nreads = crit_depth = crit_n = 0;
	why_v[0] = 0;
	rfault_sp = -1;
	rfault_off = 0;
	wfault_n = ignore_from = 0;
	ignore_off = 0;
	ignore_any = 0;
	nack_at = timeout_at = rxtimeout_at = abort_at = err_at = 0;
	ist_sticky = busy_after_read = 0;
	ticks = 1000;
	tick_step = 100;
	ops.rd32 = f_rd32;
	ops.wr32 = f_wr32;
	ops.now = f_now;
	ops.crit = f_crit;
	ops.say = f_say;
	ops.freq_hz = 24576000ull;
	ops.ctx = NULL;
	st.stopped = 0;
	st.sets = 0;
	h.mode = 0x4; h.ipclk = 0x00200000; h.pclk = 0x00200000; h.qch = 0x003F0002;
	h.pcon = 0x00220000u; h.pdat = 0x30u;
	h.ctl = 0x48; h.fifo = 0x00080083; h.trail = 0x00FFFFFF; h.conf = 0x980110FF; h.aconf = 0x00030001; h.tmo = 0xFF;
	h.trans = 0x00080001; h.addr = 0x00009400; h.inten = 0x80; h.ist = 0; h.err = 0; h.usicon = 0;
	memset(fgv, 0, sizeof(fgv));
	memset(fgv_known, 0, sizeof(fgv_known));
	fgv[0x06] = 0x3E6D;	/* oriole-fg1: 62.42 % */
	fgv[0x08] = 0x1D79;	/* 29.4 C */
	fgv[0x09] = 0xC6B8;	/* 3974 mV */
	fgv[0x0A] = 0xFE20;	/* chosen: -480 * 312.5 uA */
	fgv[0x0B] = 0xFE31;	/* -144.6 mA */
	fgv[0x35] = 0x082F;
	fgv[0x21] = 0x6200;	/* DevName reset value of the register table (max_m5_reg.h:545) */
	for (unsigned i = 0; i < 7; i++)
		fgv_known[SEVEN[i]] = 1;
}

static int has(const char *s)
{
	for (unsigned i = 0; i < nl; i++)
		if (strstr(lines[i], s))
			return 1;
	return 0;
}

static unsigned count(const char *s)
{
	unsigned n = 0;

	for (unsigned i = 0; i < nl; i++)
		n += strstr(lines[i], s) != NULL;
	return n;
}

static int fails;

static void dump_fail(int line, const char *expr)
{
	fprintf(stderr, "FAIL line %d: %s (violations %u: %s)\n", line, expr, violations, why_v);
	for (unsigned i = 0; i < nl; i++)
		fprintf(stderr, "  %s\n", lines[i]);
	for (unsigned j = 0; j < nw; j++)
		fprintf(stderr, "  w %02x <- %08x\n", wlog[j].off, wlog[j].v);
	fails++;
	exit(1);
}

#define EXPECT(c) do { if (!(c)) dump_fail(__LINE__, #c); } while (0)

static int as_before(void)
{
	return h.ctl == 0x48 && h.fifo == 0x00080083 && h.inten == 0x80 && h.addr == 0x00009400 && h.aconf == 0x00030001 && h.ist == 0 &&
	       h.trail == 0x00FFFFFF && h.conf == 0x980110FF && h.tmo == 0xFF;
}

static int restore_tail(void)
{
	static const uint32_t off[6] = { R_AUTO, R_ADDR, R_CTL, R_FIFO_CTL, R_INT_EN, R_INT_ST };
	static const uint32_t v[5] = { 0x00030001, 0x00009400, 0x48, 0x00080083, 0x80 };

	if (nw < 6)
		return 0;
	for (unsigned i = 0; i < 6; i++)
		if (wlog[nw - 6 + i].off != off[i] || (i < 5 && wlog[nw - 6 + i].v != v[i]))
			return 0;
	return 1;
}

/* the expected 17 writes of one register (the shim's sequence) */
static int expect_one(unsigned k, uint8_t reg)
{
	static const uint32_t off[17] = { R_FIFO_CTL, R_INT_ST, R_ADDR, R_CTL, R_INT_EN, R_AUTO, R_AUTO, R_TX, R_INT_ST,
					  R_FIFO_CTL, R_INT_ST, R_ADDR, R_CTL, R_INT_EN, R_AUTO, R_AUTO, R_INT_ST };
	const uint32_t v[17] = { 0x00010013, 0, 0x0000D800, 0x88, 0x80, 0x00000001, 0x80000001, reg, DONE,
				 0x00020023, 0, 0x0000D800, 0x48, 0x80, 0x00030002, 0x80030002, DONE };

	for (unsigned i = 0; i < 17; i++, k++)
		if (k >= nw || wlog[k].off != off[i] || wlog[k].v != v[i]) {
			fprintf(stderr, "write %u: want %02x <- %08x\n", k, off[i], v[i]);
			return 0;
		}
	return 1;
}

/* a refusal: return 1, ZERO writes, no transfer, the reason, the reader stopped; a second set performs no access at all */
static void refused(const char *why)
{
	unsigned r0;

	EXPECT(sfg_set(&st, &ops, &cfg) == 1);
	EXPECT(nw == 0 && nruns == 0 && violations == 0 && crit_n == 0);
	EXPECT(has(why));
	EXPECT(has("SWEET BATTERY: refused "));
	EXPECT(!has("!!!") && !has("src=fg"));
	EXPECT(st.stopped == 1);
	r0 = nreads;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && nreads == r0 && nw == 0);
}

static void cfg_refused(const char *name, uint32_t val, int drop, const char *why)
{
	const char *r;

	ov_name = name; ov_val = val; ov_drop = drop;
	r = load();
	ov_name = NULL; ov_drop = 0;
	if (!r || !strstr(r, why)) {
		fprintf(stderr, "FAIL cfg %s=%#x drop=%d: got %s, want %s\n", name, val, drop, r ? r : "(accepted)", why);
		exit(1);
	}
}

int main(void)
{
	/* the loader on the mkadt bytes, and the derived values against the cited ones */
	EXPECT(load() == NULL);
	EXPECT(cfg.i2c_pa == 0x10D50000ull && cfg.pad_pa == 0x10C40060ull && cfg.mode_pa == 0x10C21010ull && cfg.cmu_pa == 0x10C00000ull);
	EXPECT(cfg.o_ctl == R_CTL && cfg.o_fifo_ctl == R_FIFO_CTL && cfg.o_int_en == R_INT_EN && cfg.o_int_st == R_INT_ST && cfg.o_addr == R_ADDR &&
	       cfg.o_auto == R_AUTO && cfg.o_tx == R_TX && cfg.o_rx == R_RX && cfg.o_fifo_st == R_FIFO_ST && cfg.o_trans == R_TRANS &&
	       cfg.o_err == R_ERR && cfg.o_conf == R_CONF && cfg.o_tmo == R_TMO && cfg.o_trail == R_TRAIL && cfg.o_usicon == R_USICON);
	EXPECT(cfg.v_ctl_tx == 0x88 && cfg.v_ctl_rx == 0x48 && cfg.v_fifo_w == 0x00010013 && cfg.v_fifo_r == 0x00020023 && cfg.v_int_en == 0x80 &&
	       cfg.v_addr == 0x0000D800 && cfg.v_auto_w == 1 && cfg.v_auto_r == 0x00030002 && cfg.v_run == RUN);
	for (unsigned i = 0; i < 7; i++)
		EXPECT(cfg.regs[i] == SEVEN[i] && sfg_reg_ptr(i) == SEVEN[i]);
	EXPECT(sfg_reg_ptr(SFG_NREGS) == 0xFFu && !sfg_is_reg_ptr(0xFFu));

	/* happy path: seven reads, each = pointer write (no STOP) + 2-byte read (STOP), then the restore; the oriole-fg1 numbers */
	world();
	EXPECT(sfg_set(&st, &ops, &cfg) == 0);
	EXPECT(violations == 0 && crit_depth == 0 && crit_n == 8);
	EXPECT(nruns == 14);
	EXPECT(nw == 7 * 17 + 6);
	for (unsigned i = 0; i < 7; i++)
		EXPECT(expect_one(17 * i, SEVEN[i]));
	EXPECT(restore_tail() && as_before());
	EXPECT(has("SWEET BATTERY: before CTL=00000048 FIFO_CTL=00080083 INT_EN=00000080"));
	EXPECT(has("SWEET BATTERY: before CONF=980110FF TIMEOUT=000000FF TRAILING=00FFFFFF"));
	EXPECT(has("SWEET BATTERY: raw 06=3E6D 08=1D79 09=C6B8 0A=FE20 0B=FE31 35=082F 21=6200 restore=ok"));
	EXPECT(has("SWEET BATTERY: restore wait idle=1") && !has("SWEET BATTERY: fail "));
	EXPECT(count("SWEET BATTERY: soc=62.42% v=3974 mV i=-144.6 mA t=29.4 C src=fg") == 1);
	EXPECT(!has("!!!") && !has("refused") && st.stopped == 0 && st.sets == 1);
	/* the next set (30 s later): the same writes, ONE line only */
	nw = 0; nl = 0; nruns = 0;
	EXPECT(sfg_set(&st, &ops, &cfg) == 0 && violations == 0 && nw == 7 * 17 + 6 && nl == 1 && st.sets == 2);
	EXPECT(!strcmp(lines[0], "SWEET BATTERY: soc=62.42% v=3974 mV i=-144.6 mA t=29.4 C src=fg"));

	/* MASTER_RUN stays set after a transfer (the other guess) -> the same writes, still ok */
	world(); h.run_sticks = 1;
	EXPECT(sfg_set(&st, &ops, &cfg) == 0 && violations == 0 && nw == 7 * 17 + 6 && restore_tail());

	/* decoding: signs and limits */
	world(); fgv[0x08] = 0xFE80; fgv[0x06] = 0x0140; fgv[0x0B] = 0x0010; fgv[0x09] = 0x0000;
	EXPECT(sfg_set(&st, &ops, &cfg) == 0 && has("soc=1.25% v=0 mV i=+5.0 mA t=-1.5 C src=fg"));
	world(); fgv[0x08] = 0xFFE6; fgv[0x06] = 0x0001; fgv[0x0B] = 0xFFFF;
	EXPECT(sfg_set(&st, &ops, &cfg) == 0 && has("soc=0.00% v=3974 mV i=-0.3 mA t=-0.1 C src=fg"));
	world(); fgv[0x06] = 0xFFFF; fgv[0x09] = 0xFFFF; fgv[0x0B] = 0x0000; fgv[0x08] = 0x0000;
	EXPECT(sfg_set(&st, &ops, &cfg) == 0 && has("soc=255.99% v=5119 mV i=+0.0 mA t=0.0 C src=fg"));

	/* refusals: ZERO writes, the reason, stopped */
	world(); ops.freq_hz = 0; refused("refused no timer");
	world(); tick_step = 0; refused("refused counter not running");
	world(); h.mode = 0x0; refused("refused i2c12 gate mode-not-selected");
	world(); h.mode = 0x2; refused("refused i2c12 gate mode-not-selected");
	world(); h.ipclk = 0; refused("refused i2c12 gate ipclk-gated");
	world(); h.pclk = 0; refused("refused i2c12 gate pclk-gated");
	world(); h.qch = 0; refused("refused i2c12 gate qch-no-clkreq");
	world(); rfault_sp = SFG_SP_MODE; rfault_off = 0; refused("refused i2c12 gate sysreg-unreadable");
	world(); rfault_sp = SFG_SP_CMU; rfault_off = CMU_PCLK; refused("refused i2c12 gate cmu-unreadable");
	world(); h.pcon = 0x00020000u; refused("refused I2C12 pads not in the I2C function");
	world(); h.pcon = 0x00200000u; refused("refused I2C12 pads not in the I2C function");
	world(); h.pdat = 0x10u; refused("refused I2C12 pads not in the I2C function");
	world(); h.pdat = 0x20u; refused("refused I2C12 pads not in the I2C function");
	world(); rfault_sp = SFG_SP_PAD; rfault_off = PAD_DAT; refused("refused I2C12 pads unreadable");
	world(); rfault_sp = SFG_SP_I2C; rfault_off = R_TRANS; refused("refused controller unreadable");
	world(); rfault_sp = SFG_SP_I2C; rfault_off = R_USICON; refused("refused controller unreadable");
	world(); h.trans |= MBUSY; refused("refused bus busy");
	world(); h.trans |= 0x10000; refused("refused bus busy");
	world(); h.trans = (h.trans & ~0xfu) | 0xC; refused("refused master state");
	world(); h.ist = DONE; refused("refused INT_STATUS pending");
	world(); h.err = 4; refused("refused ERR_STATUS nonzero");
	world(); h.nrx = 1; refused("refused FIFO not empty");
	world(); h.ctl = 0x49; refused("refused CTL");
	world(); h.ctl = 0x40; refused("refused CTL");
	world(); h.conf |= 1u << 29; refused("refused CONF");
	world(); h.conf = 0x180110FF; refused("refused CONF");
	world(); h.aconf |= RUN; refused("refused AUTO_CONF");
	world(); h.aconf |= 1u << 20; refused("refused AUTO_CONF");
	world(); h.tmo |= 1u << 31; refused("refused TIMEOUT");
	world(); h.trail = 0xf; refused("refused TRAILING_CTL");
	world(); h.addr = 0x00009401; refused("refused ADDR");
	world(); h.usicon = 1; refused("refused USI");
	/* XNU runs later than the shim: a state that passes the shim's checks but is not the observed handoff is refused too */
	world(); h.ctl = 0x88; refused("refused handoff: CTL");
	world(); h.fifo = 0x00010013; refused("refused handoff: FIFO_CTL");
	world(); h.inten = 0; refused("refused handoff: INT_ENABLE");
	world(); h.conf = 0x980110FE; refused("refused handoff: CONF");
	world(); h.aconf = 0x00030002; refused("refused handoff: AUTO_CONF");
	world(); h.tmo = 0x7F; refused("refused handoff: TIMEOUT");
	world(); h.addr = 0x0000D800; refused("refused handoff: ADDR");	/* a leftover 0x36 address is not the bootloader's */
	world(); h.usicon = 2; refused("refused handoff: USI_CON");

	/* NACK on the very first transfer: stop, restore, refused, stopped */
	world(); nack_at = 1;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && violations == 0 && nruns == 1 && nw == 7 + 6 && restore_tail() && as_before());
	EXPECT(has("SWEET BATTERY: refused REPSOC reg=0x06 nack restore=ok") && has("raw 06=---- 08=----") && !has("src=fg") && st.stopped);
	/* review F5 (0148): the failure-time status words, the step and the poll condition, from reads the reader already made */
	EXPECT(has("SWEET BATTERY: fail at ptr-txroom why=nack-int") && has("SWEET BATTERY: fail INT_STATUS=") && has("SWEET BATTERY: fail POLLS="));
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && nruns == 1 && nw == 7 + 6);
	/* NACK on the read of the third register: two good reads, then the restore (bus held -> restore=FAILED, loud) */
	world(); nack_at = 6;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && violations == 0 && nruns == 6 && nw == 2 * 17 + 9 + 7 + 6 && restore_tail() && as_before());
	EXPECT(has("refused VCELL reg=0x09 nack restore=FAILED") && has("raw 06=3E6D 08=1D79 09=---- 0A=----") && has("!!! RESTORE FAILED"));
	EXPECT(has("restore wait idle=0") && has("!!! BUS NOT IDLE"));
	/* the bus goes idle late (the wait covers it), or never (timeout) */
	world(); busy_after_read = 3;
	EXPECT(sfg_set(&st, &ops, &cfg) == 0 && violations == 0 && has("src=fg"));
	world(); busy_after_read = -1;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && has("refused REPSOC reg=0x06 timeout restore=FAILED") && has("!!! BUS NOT IDLE") && as_before());
	/* timeout: the pointer byte never completes -> bounded by the counter, restore, refused */
	world(); timeout_at = 1;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && violations == 0 && nw == 8 + 6 && restore_tail() && as_before());
	EXPECT(has("refused REPSOC reg=0x06 timeout"));
	EXPECT(has("fail at ptr-txdone why=none"));
	EXPECT(ticks - 1000 >= 24576000ull / 100);		/* waited at least 10 ms of counter */
	EXPECT(ticks - 1000 <= 4 * (24576000ull / 100));	/* the counter ended it, not the poll cap */
	EXPECT(nreads < 4u * 2500u * 4u);	/* ~2458 polls of 4 reads per 10 ms wait at 100 ticks per counter read: the counter bound, not the 100000-poll cap */
	world(); rxtimeout_at = 4;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && violations == 0 && nw == 17 + 9 + 7 + 6 && restore_tail() && has("refused TEMP reg=0x08 timeout"));
	/* the poll count bounds the wait even when the counter barely moves */
	world(); timeout_at = 1; tick_step = 1;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && has("timeout") && restore_tail());
	/* arbitration lost / ERR_STATUS */
	world(); abort_at = 2;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && violations == 0 && has("refused REPSOC reg=0x06 error") && restore_tail() && as_before());
	EXPECT(has("fail at rd-byte0 why=int-errs") && count("SWEET BATTERY: fail ") == 3);
	world(); err_at = 3;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && violations == 0 && nw == 17 + 7 + 6 && has("refused TEMP reg=0x08 error restore=FAILED"));
	EXPECT(has("fail at ptr-txroom why=err-status ERR=00000001"));
	/* a faulting write (the 3rd: ADDR): error, restore still runs */
	world(); wfault_n = 3;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && violations == 0 && nruns == 0 && restore_tail() && has("refused REPSOC reg=0x06 error restore=ok"));
	EXPECT(has("fail at ptr-start why=none"));
	/* INT_STATUS stays set: the read message cannot start; the step names it */
	world(); ist_sticky = 1;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && has("fail at rd-start"));
	/* every write accepted but dropped (CR-07): the AUTO_CONF read-back differs -> never ok */
	world(); ignore_from = 1; ignore_any = 1;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && nruns == 0 && has("refused") && !has("src=fg"));
	/* the restore fails: the ADDR restore write is dropped -> LOUD, values printed, then refused and stopped */
	world(); ignore_from = 7 * 17 + 1; ignore_off = R_ADDR;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && h.addr == 0x0000D800);
	EXPECT(has("!!! RESTORE FAILED") && has("restore=FAILED") && has("restore ADDR=0000D800") && has("refused restore=FAILED") && st.stopped);
	world(); ignore_from = 7 * 17 + 1; ignore_off = R_FIFO_CTL;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && has("refused restore=FAILED"));
	/* INT_STATUS does not clear: the next message cannot start on a stale TRANS_DONE, restore FAILED */
	world(); ist_sticky = 1;
	EXPECT(sfg_set(&st, &ops, &cfg) == 1 && nruns == 1 && has("refused REPSOC reg=0x06 error restore=FAILED") && has("!!! RESTORE FAILED"));

	/* the writer's allowlist: every other offset refused with ZERO bus accesses; listed registers only with the exact values */
	{
		struct sfg_run r;

		world();
		r.o = &ops; r.c = &cfg; r.d = NULL;
		memset(&r.s, 0, sizeof(r.s));
		r.s.ctl = 0x48; r.s.fifo = 0x00080083; r.s.inten = 0x80; r.s.addr = 0x9400; r.s.aconf = 0x00030001;
		crit_depth = 1;
		nreads = 0;
		for (uint32_t off = 0; off < 0x1000; off += 1) {
			int listed = off == 0x00 || off == 0x04 || off == 0x20 || off == 0x24 || off == 0x34 || off == 0x44 || off == 0x70;

			if (listed)
				continue;
			EXPECT(sfg_w32(&r, off, 0) == -2 && sfg_w32(&r, off, 0xffffffffu) == -2);
		}
		EXPECT(sfg_w32(&r, 0xFFFFFFFCu, 0) == -2);
		EXPECT(nw == 0 && nreads == 0 && violations == 0);
		EXPECT(sfg_w32(&r, 0x00, 0xC8) == -2 && sfg_w32(&r, 0x00, 0x8) == -2 && sfg_w32(&r, 0x00, 0x80000048) == -2);
		EXPECT(sfg_w32(&r, 0x04, 0x00010003) == -2 && sfg_w32(&r, 0x04, 0) == -2);
		EXPECT(sfg_w32(&r, 0x20, 0x0F80) == -2 && sfg_w32(&r, 0x20, 0) == -2);
		/* no other device: every 7-bit address but 0x36 in the master field is refused (0x69 charger, 0x66 PMIC, 0x57, 0x35 OVP). The one
		 * exception is the snapshot itself: the bootloader leaves ADDR = 0x9400 (= 0x25 << 10, the TCPC, runs oriole-peri1/fg1), and the
		 * restore writes it back WITHOUT MASTER_RUN (no transfer; RUN needs ADDR = 0xD800, checked below), exactly as the shim does. */
		for (uint32_t a = 0; a < 0x80; a++)
			if (a != 0x36 && a != 0x25)
				EXPECT(sfg_w32(&r, 0x70, a << 10) == -2);
		EXPECT(nw == 0);
		EXPECT(sfg_w32(&r, 0x70, 0x69u << 10) == -2 && sfg_w32(&r, 0x70, 0x66u << 10) == -2 &&
		       sfg_w32(&r, 0x70, 0x57u << 10) == -2 && sfg_w32(&r, 0x70, 0x35u << 10) == -2 && sfg_w32(&r, 0x70, 0x0000D801) == -2 &&
		       sfg_w32(&r, 0x70, 0x0700D800) == -2);
		/* AUTO_CONF: no write message with data bytes (length 2 without READ_WRITE), no RUN without the config write and ADDR = 0x36 */
		EXPECT(sfg_w32(&r, 0x44, 0x00000002) == -2 && sfg_w32(&r, 0x44, 0x80000002) == -2 && sfg_w32(&r, 0x44, 0x00020002) == -2 &&
		       sfg_w32(&r, 0x44, 0x00030001 | RUN) == -2 && sfg_w32(&r, 0x44, 0x00000003) == -2);
		EXPECT(sfg_w32(&r, 0x44, 0x80000001) == -2);	/* RUN refused: no config write before (AUTO_CONF 0x00030001) */
		h.aconf = 0x00000001;
		EXPECT(sfg_w32(&r, 0x44, 0x80000001) == -2);	/* RUN refused while ADDR is not the fuel gauge */
		h.aconf = 0x00030002; h.addr = 0x0000D801;
		EXPECT(sfg_w32(&r, 0x44, 0x80030002) == -2);
		h.aconf = 0x00030001; h.addr = 0x00009400;
		/* TX_DATA: only the seven pointers */
		for (uint32_t v = 0; v < 0x200; v++) {
			int ok = 0;

			for (unsigned i = 0; i < 7; i++)
				ok |= v == SEVEN[i];
			if (!ok)
				EXPECT(sfg_w32(&r, 0x34, v) == -2);
		}
		h.ist = 0x80;
		EXPECT(sfg_w32(&r, 0x24, 0x280) == -2 && sfg_w32(&r, 0x24, 0) == -2 && sfg_w32(&r, 0x24, 0xffffffffu) == -2);
		EXPECT(nw == 0 && violations == 0);
		/* the restore entries are the snapshot values only */
		EXPECT(sfg_w32(&r, 0x70, 0x9400) == 0 && sfg_w32(&r, 0x70, 0x9800) == -2 && sfg_w32(&r, 0x00, 0x48) == 0 && nw == 2);
		/* RUN only after the config write read back, and only with ADDR = 0x36 */
		world(); crit_depth = 1; h.addr = 0x0000D800; h.ctl = 0x88;
		EXPECT(sfg_w32(&r, 0x44, 0x80000001) == -2);
		EXPECT(sfg_w32(&r, 0x44, 0x00000001) == 0 && sfg_w32(&r, 0x44, 0x80030002) == -2 && sfg_w32(&r, 0x44, 0x80000001) == 0 && nruns == 1);
		EXPECT(sfg_w32(&r, 0x34, 0x09) == 0 && violations == 0);
		/* a snapshot holding MASTER_RUN can never be restored */
		r.s.aconf = 0x80030001;
		EXPECT(sfg_w32(&r, 0x44, 0x80030001) == -2);
		/* the read allowlist: TX_DATA never, RX_DATA not through sfg_r32, nothing outside the listed words */
		nreads = 0;
		for (uint32_t off = 0; off < 0x1000; off += 4) {
			uint32_t v;
			int listed = off == 0x00 || off == 0x04 || off == 0x08 || off == 0x20 || off == 0x24 || off == 0x2C || off == 0x30 || off == 0x40 ||
				     off == 0x44 || off == 0x48 || off == 0x50 || off == 0x70 || off == 0xC4;

			EXPECT(sfg_rd_ok(&cfg, SFG_SP_I2C, off) == listed);
			if (!listed)
				EXPECT(sfg_r32(&r, SFG_SP_I2C, off, &v) == 0);
		}
		EXPECT(nreads == 0);
		EXPECT(!sfg_rd_ok(&cfg, SFG_SP_MODE, 4) && !sfg_rd_ok(&cfg, SFG_SP_CMU, 0x2034) && !sfg_rd_ok(&cfg, SFG_SP_PAD, 0x8) && !sfg_rd_ok(&cfg, 9, 0));
		crit_depth = 0;
	}

	/* the config loader refuses ADT values outside the allowlist or inconsistent ones */
	cfg_refused("fg-addr7", 0x69, 0, "fg-addr7 is not the allowlisted device");
	cfg_refused("fg-addr7", 0x37, 0, "fg-addr7 is not the allowlisted device");
	cfg_refused("fg-regs", 0x07, 0, "fg-regs is not the allowlisted register list");
	cfg_refused("fg-regs", 0, 1, "property fg-regs missing");
	cfg_refused("off-conf", 0x44, 0, "two controller offsets are equal");
	cfg_refused("off-tx-data", 0x35, 0, "unaligned");
	cfg_refused("off-usi-con", 0xC8, 0, "outside reg");
	cfg_refused("bit-auto-master-run", 32, 0, "a bit number is not 0..31");
	cfg_refused("bit-ctl-rxchon", 7, 0, "two bits of one register are equal");
	cfg_refused("fld-addr-mas-shift", 30, 0, "a field is out of range");
	cfg_refused("fld-auto-len-mask", 0x1FFFF, 0, "AUTO_CONF length field overlaps");
	cfg_refused("pin-sda", 4, 0, "pad description out of range");
	cfg_refused("pin-scl", 8, 0, "pad description out of range");
	cfg_refused("cmu-qch-off", 0x3034, 0, "gate description out of range");
	cfg_refused("usi-mode-val", 0x8, 0, "gate description out of range");
	cfg_refused("handoff-addr", 0x9401, 0, "the handoff words fail");
	cfg_refused("handoff-auto-conf", 0x80030001, 0, "the handoff words fail");
	cfg_refused("handoff-ctl", 0x49, 0, "the handoff words fail");
	cfg_refused("fg-vcell-nv-per-lsb", 0, 0, "a unit scale is zero");
	cfg_refused("reg", 0, 1, "property reg missing");
	cfg_refused("cmu-reg", 0, 1, "property cmu-reg missing");
	EXPECT(load() == NULL);

	puts("PASS hsi2c_fg_xnu host test");
	return fails;
}
