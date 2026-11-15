/* Host test for xnu patch 0158 (bsd/dev/sweet_amp.h, boot-arg sweetamp=1): the A1 identify of shim/ampid_host_test.c (same fake SoC and fake CS35L41,
 * typed from the driver text) plus the A7 checks: every write frame is one of the 12 stock B2 probe writes in stock order (cs35l41.c:4300-4302,
 * :3675-3686, :4401), only after that amp was identified, never PWR_CTRL1; an error bit in IRQ1_STATUS1 fails the init; the armed-write allowlist.
 * The header is extracted from the patch by shim/build.sh:
 *   gcc -O1 -Wall -Wextra -Werror -Wno-unused-function -I$O -o $O/amp_xnu_host_test amp_xnu_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sweet_amp.h"
#define WA(pa, o, v, a, r) w32_allowed((pa), (o), (v), (a), (r), 0, 0, 0)
/* ---- the independent constants (typed from the sources, never from ampid.c) ---- */
#define B		0x10D60000UL	/* spi_13 (REF dt spi@10D60000 reg) */
#define R_CH		(B + 0x00)	/* spi-s3c64xx.c :40-51 */
#define R_CLK		(B + 0x04)
#define R_MODE		(B + 0x08)
#define R_SLAVE		(B + 0x0C)
#define R_INTEN		(B + 0x10)
#define R_STATUS	(B + 0x14)
#define R_TX		(B + 0x18)
#define R_RX		(B + 0x1C)
#define R_PKT		(B + 0x20)
#define R_PND		(B + 0x24)
#define R_SWAP		(B + 0x28)
#define R_FB		(B + 0x2C)
#define R_USICON	(B + 0xC4)
#define R_USIOPT	(B + 0xC8)
#define SYSREG		0x10C21014UL	/* sysreg_peric1 0x10C21000 + usi-offset 0x14 */
#define MUX		0x10C00660UL	/* PLL_CON0_MUX_CLKCMU_PERIC1_USI13_USI_USER */
#define DIV		0x10C01814UL
#define IPCLK		0x10C02034UL
#define PCLK		0x10C02054UL
#define QCH		0x10C03034UL
#define G25		0x10C400A0UL	/* gpp25: CLK 0, MOSI 1, MISO 2, CS0 3 */
#define G1		0x10840020UL	/* gpp1: CS1 0 */
#define G16		0x10840200UL	/* gpp16: RST_L 3 (BT_REG_ON is pin 2) */
#define G23		0x10C40060UL	/* gpp23: RST_R 7 (I2C12 SCL/SDA are pins 4/5) */
#define GA6		0x174E0000UL	/* gpa6: IRQ_L 3 */
#define GA9		0x174D00C0UL	/* gpa9: IRQ_R 6 */
#undef CH_TX
#undef CH_RX
#undef CH_CPHA
#undef CH_CPOL
#undef CH_SLAVE
#undef CH_SWRST
#undef PKT_EN		/* ampid.c names them too; the test types its own values */
#define CH_TX		0x01u
#define CH_RX		0x02u
#define CH_CPHA		0x04u
#define CH_CPOL		0x08u
#define CH_SLAVE	0x10u
#define CH_SWRST	0x20u
#define PKT_EN		0x10000u

enum { NB = 6 };
static const uintptr_t BANKS[NB] = { G25, G1, G16, G23, GA6, GA9 };
static const uint32_t REGS4[4] = { 0x1001Cu, 0x10018u, 0x0u, 0x4u };

static struct {
	uint32_t sys, mux, div, ipclk, pclk, qch;
	uint32_t con[NB], dat[NB], pud[NB], drv[NB];
	uint32_t usicon, ch, clk, mode, slave, inten, pkt, swap, fb, usiopt;
	uint8_t tx[128]; unsigned ntx;
	uint8_t rx[128]; unsigned nrx;
	unsigned mux_busy;
} h, h0;

struct amp {
	uint32_t mask1, ccm, st1;	/* IRQ1_MASK1, DSP1_CCM_CORE_CTRL, IRQ1_STATUS1 */
	unsigned nwr, seq_bad;	/* write frames seen, out-of-sequence writes */
	int booted;		/* OTP done after boot_polls status reads since the reset release */
	unsigned boot_polls, reads_since;
	int absent;		/* no MISO answer: reads give 0 */
	int ff;			/* MISO stuck high */
	uint8_t frame[16]; unsigned nf;
	uint64_t t_low, t_high;	/* tick of the last reset assert (output low) / release */
	int was_released;
	unsigned frames;
};
static struct amp amp[2];
static unsigned violations;
static const uint32_t SEQ[12][2] = { { 0x10110u, 0x7FFCFE3Fu }, { 0x40u, 0x5555u }, { 0x40u, 0xAAAAu }, { 0x4100u, 0u }, { 0x4310u, 0u }, { 0x4400u, 0u },
				     { 0x381Cu, 0x51u }, { 0x2BC20E0u, 0u }, { 0x2BC2020u, 0u }, { 0x40u, 0xCCCCu }, { 0x40u, 0x3333u }, { 0x2BC1000u, 0u } };
static char why_v[256];
static unsigned nw, nreads, nframes;
static uintptr_t rfault_pa;
static unsigned rfault_after;		/* the fault applies only after this many reads of rfault_pa */
static unsigned rfault_cnt;
static unsigned wfault_n;		/* 1-based write index that faults (not applied) */
static unsigned ignore_from;		/* writes from this 1-based index on are dropped (accepted) */
static int no_clock;			/* the bus never clocks (RX never fills) */
static uint64_t ticks, tick_step, freq;
static uint64_t first_frame_tick[2];
static char lines[512][RAMLOG_LINE_MAX + 8];
static unsigned nl;
static uint32_t ch_snap_inject;
static unsigned mux_writes;

static void violate(const char *s)
{
	if (!violations)
		snprintf(why_v, sizeof(why_v), "%s", s);
	violations++;
}

static uint64_t counter_freq(void) { return freq; }
static uint64_t touch_ticks(void) { ticks += tick_step; return ticks; }

static void con_str(const char *label, const char *s)
{
	assert(nl < 512);
	assert(strlen(label) + 1 + strlen(s) < RAMLOG_LINE_MAX);
	snprintf(lines[nl++], sizeof(lines[0]), "%s %s", label, s);
}

static int bank_of(uintptr_t pa, unsigned *reg)
{
	for (int i = 0; i < NB; i++)
		if (pa >= BANKS[i] && pa < BANKS[i] + 0x10 && !(pa & 3)) {
			*reg = (unsigned)(pa - BANKS[i]) / 4u;
			return i;
		}
	return -1;
}

static uint32_t field(uint32_t w, unsigned nr) { return (w >> (4 * nr)) & 0xf; }
static uint32_t bit(uint32_t w, unsigned nr) { return (w >> nr) & 1; }

/* a pad is a driven output low: CON 1 and DAT 0 */
static int out_low(int b, unsigned nr) { return field(h.con[b], nr) == 1 && !bit(h.dat[b], nr); }
static int cs_low(int a) { return a == 0 ? out_low(0, 3) : out_low(1, 0); }
/* the amp is out of reset only while its reset pad is an output driven high */
static int released(int a) { int b = a == 0 ? 2 : 3; unsigned nr = a == 0 ? 3 : 7; return field(h.con[b], nr) == 1 && bit(h.dat[b], nr); }

static uint32_t status(void)
{
	return ((uint32_t)h.ntx << 6) | ((uint32_t)h.nrx << 15);	/* TX level [12:6], RX level [21:15] (rx_lvl_offset 15) */
}

static int spi_selected(void) { return (h.sys & 7u) == 2u; }

static int try_read32(uintptr_t pa, uint32_t *v)
{
	unsigned r;
	int b;

	nreads++;
	if (pa == rfault_pa && ++rfault_cnt > rfault_after)
		return 0;
	if (pa == SYSREG) { *v = h.sys; return 1; }
	if (pa == MUX) { *v = h.mux | (h.mux_busy ? 0x10000u : 0u); if (h.mux_busy) h.mux_busy--; return 1; }
	if (pa == DIV) { *v = h.div; return 1; }
	if (pa == IPCLK) { *v = h.ipclk; return 1; }
	if (pa == PCLK) { *v = h.pclk; return 1; }
	if (pa == QCH) { *v = h.qch; return 1; }
	if ((b = bank_of(pa, &r)) >= 0) {
		*v = r == 0 ? h.con[b] : r == 1 ? h.dat[b] : r == 2 ? h.pud[b] : h.drv[b];
		return 1;
	}
	if (pa >= B && pa < B + 0x100) {
		if (!spi_selected()) { violate("SPI13 controller read while the USI13 mode is not SPI"); return 0; }
		if (pa == R_CH) { *v = h.ch; return 1; }
		if (pa == R_CLK) { *v = h.clk; return 1; }
		if (pa == R_MODE) { *v = h.mode; return 1; }
		if (pa == R_SLAVE) { *v = h.slave; return 1; }
		if (pa == R_INTEN) { *v = h.inten; return 1; }
		if (pa == R_STATUS) { *v = status(); return 1; }
		if (pa == R_PKT) { *v = h.pkt; return 1; }
		if (pa == R_SWAP) { *v = h.swap; return 1; }
		if (pa == R_FB) { *v = h.fb; return 1; }
		if (pa == R_USICON) { *v = h.usicon; return 1; }
		if (pa == R_USIOPT) { *v = h.usiopt; return 1; }
		if (pa == R_RX) {
			if (!h.nrx) { violate("RX_DATA read with the RX FIFO empty"); *v = 0; return 1; }
			*v = h.rx[0];
			memmove(h.rx, h.rx + 1, --h.nrx);
			return 1;
		}
		violate("read of TX_DATA / PENDING_CLR / an unlisted SPI word");
		return 0;
	}
	violate("read outside the listed words");
	return 0;
}

/* the amp sees one byte on MOSI while its chip select is low; returns its MISO byte */
static int amp_released_ok(int a)
{
	return released(a) && amp[a].booted;
}

static uint8_t amp_byte(int a, uint8_t mosi, int tx_on)
{
	struct amp *m = &amp[a];
	unsigned p = m->nf;
	uint32_t addr, val;

	if (m->nf < sizeof(m->frame))
		m->frame[m->nf++] = mosi;
	if (p == 0) {
		m->frames++;
		nframes++;
		if (!first_frame_tick[a])
			first_frame_tick[a] = ticks;
		if (!(mosi & 0x80u) && !amp_released_ok(a))
			violate("a write frame to an amp that is not out of reset and identified");
		if (!tx_on)
			violate("a frame starts with the transmitter off");
	}
	if (!(m->frame[0] & 0x80u)) {		/* a write frame: 4 address, 2 pad, 4 value bytes, all clocked out */
		if (!tx_on)
			violate("a write frame byte with the transmitter off");
		if (p >= 4 && p < 6 && mosi != 0)
			violate("a write frame pad byte is not 0");
		if (p == 9) {
			uint32_t wa = (uint32_t)m->frame[0] << 24 | (uint32_t)m->frame[1] << 16 | (uint32_t)m->frame[2] << 8 | m->frame[3];
			uint32_t wv = (uint32_t)m->frame[6] << 24 | (uint32_t)m->frame[7] << 16 | (uint32_t)m->frame[8] << 8 | m->frame[9];

			if (wa == 0x2014u)
				violate("PWR_CTRL1 (GLOBAL_EN) written");
			if (m->nwr >= 12 || SEQ[m->nwr][0] != wa || SEQ[m->nwr][1] != wv) {
				m->seq_bad++;
				violate("a write outside the stock B2 sequence or out of order");
			}
			m->nwr++;
			if (wa == 0x10110u) m->mask1 = wv;
			if (wa == 0x2BC1000u) m->ccm = wv;
		}
		if (p > 9)
			violate("a write frame longer than 10 bytes");
		return 0;
	}
	if (p < 6 && !tx_on)
		violate("an address / pad byte clocked with the transmitter off");
	if (p >= 4 && p < 6 && mosi != 0)
		violate("a pad byte is not 0");
	if (p == 5) {
		addr = ((uint32_t)m->frame[0] << 24 | (uint32_t)m->frame[1] << 16 | (uint32_t)m->frame[2] << 8 | m->frame[3]) & 0x7fffffffu;
		if (addr != REGS4[0] && addr != REGS4[1] && addr != REGS4[2] && addr != REGS4[3] && addr != 0x10010u && addr != 0x10110u && addr != 0x2BC1000u)
			violate("a frame names a register outside the four");
		if (released(a) && m->was_released && first_frame_tick[a] && first_frame_tick[a] - m->t_high < 2000u)
			violate("first frame less than 2 ms after the reset release");
	}
	if (p < 6 || m->absent || !released(a))
		return m->ff ? 0xff : 0;
	addr = ((uint32_t)m->frame[0] << 24 | (uint32_t)m->frame[1] << 16 | (uint32_t)m->frame[2] << 8 | m->frame[3]) & 0x7fffffffu;
	if (addr == 0x1001Cu && p == 6 && !m->booted && ++m->reads_since >= m->boot_polls)
		m->booted = 1;
	val = addr == 0x0u ? 0x35A40u : addr == 0x4u ? 0xB2u : addr == 0x1001Cu ? (m->booted ? 0x2u : 0u) : addr == 0x10110u ? m->mask1 :
	      addr == 0x2BC1000u ? m->ccm : addr == 0x10010u ? m->st1 : 0u;
	return p < 10 ? (uint8_t)(val >> (8 * (9 - p))) : 0;
}

/* CH_CFG written: if a channel is on and PACKET_CNT is enabled, clock PACKET_CNT bytes at once */
static void clock_bus(void)
{
	unsigned n = h.pkt & 0xffffu;

	if (no_clock || !(h.pkt & PKT_EN) || !(h.ch & (CH_TX | CH_RX)) || (h.ch & CH_SWRST))
		return;
	if (h.ch & (CH_CPHA | CH_CPOL | CH_SLAVE | 0x40u))
		violate("bytes clocked in a mode other than SPI mode 0 master");
	if (h.mux & 0x10u)
		violate("bytes clocked from the TOP clock source (SCK = 100 MHz)");
	if (h.usicon & 1u)
		violate("bytes clocked with USI_CON in reset");
	if (cs_low(0) && cs_low(1))
		violate("both chip selects low");
	if (!(h.ch & CH_RX))
		violate("a transfer without RXCH_ON");
	for (unsigned i = 0; i < n; i++) {
		uint8_t mosi = 0, miso = 0;
		int tx_on = !!(h.ch & CH_TX);

		if (tx_on) {
			if (!h.ntx) { violate("TX FIFO underrun (an undefined byte on MOSI)"); mosi = 0x5a; }
			else { mosi = h.tx[0]; memmove(h.tx, h.tx + 1, --h.ntx); }
		}
		for (int a = 0; a < 2; a++)
			if (cs_low(a))
				miso = amp_byte(a, mosi, tx_on);
		if (h.nrx < sizeof(h.rx))
			h.rx[h.nrx++] = miso;
	}
	h.pkt &= ~PKT_EN;	/* guess: the count is consumed; ampid writes it again per transfer anyway */
}

static void pad_write(int b, unsigned r, uint32_t v)
{
	uint32_t *w = r == 0 ? &h.con[b] : r == 1 ? &h.dat[b] : r == 2 ? &h.pud[b] : &h.drv[b];
	int before[2] = { cs_low(0), cs_low(1) }, rel[2] = { released(0), released(1) };

	if (b >= 4) { violate("write to an IRQ pad bank"); return; }
	if (r == 2) { violate("PUD write"); return; }
	*w = v;
	for (int a = 0; a < 2; a++) {
		if (before[a] && !cs_low(a))
			amp[a].nf = 0;	/* chip select released: the next frame starts fresh */
		if (!before[a] && cs_low(a))
			amp[a].nf = 0;
		if (rel[a] && !released(a)) { amp[a].booted = 0; amp[a].was_released = 0; }
		if (!rel[a] && released(a)) {
			int rb = a == 0 ? 2 : 3;
			unsigned nr = a == 0 ? 3 : 7;

			(void)rb; (void)nr;
			if (!amp[a].t_low || ticks - amp[a].t_low < 2000u)
				violate("reset released without >= 2 ms of driven low first");
			amp[a].t_high = ticks;
			amp[a].was_released = 1;
			amp[a].reads_since = 0;
			first_frame_tick[a] = 0;
		}
		if (out_low(a == 0 ? 2 : 3, a == 0 ? 3 : 7) && !amp[a].t_low)
			amp[a].t_low = ticks;
	}
}

static int try_write32(uintptr_t pa, uint32_t v)
{
	unsigned r;
	int b;

	nw++;
	if (wfault_n && nw == wfault_n)
		return 0;
	if (ignore_from && nw >= ignore_from)
		return 1;
	if (pa == SYSREG) { h.sys = v; return 1; }
	if (pa == MUX) { mux_writes++; if (((h.mux ^ v) & ~0x10u)) violate("mux bits other than SEL changed"); h.mux = v & ~0x10000u; h.mux_busy = 3; return 1; }
	if ((b = bank_of(pa, &r)) >= 0) { pad_write(b, r, v); return 1; }
	if (pa >= B && pa < B + 0x100) {
		if (!spi_selected()) { violate("SPI13 controller write while the USI13 mode is not SPI"); return 0; }
		if (pa == R_CH) {
			h.ch = v;
			if (v & CH_SWRST) { h.ntx = 0; h.nrx = 0; }
			clock_bus();
			return 1;
		}
		if (pa == R_MODE) { h.mode = v; return 1; }
		if (pa == R_SLAVE) { h.slave = v; return 1; }
		if (pa == R_INTEN) { h.inten = v; return 1; }
		if (pa == R_PKT) { h.pkt = v; return 1; }
		if (pa == R_PND) { if (v != 0x1eu && v != 0) violate("PENDING_CLR value"); return 1; }
		if (pa == R_SWAP) { h.swap = v; return 1; }
		if (pa == R_FB) { h.fb = v; return 1; }
		if (pa == R_USICON) { h.usicon = v; return 1; }
		if (pa == R_TX) {
			if (v > 0xffu) violate("a TX write wider than a byte");
			if (h.ntx < 64) h.tx[h.ntx++] = (uint8_t)v; else violate("TX FIFO overflow");
			return 1;
		}
		violate("write to CLK_CFG / STATUS / RX_DATA / USI_OPTION / an unlisted SPI word");
		return 0;
	}
	violate("write outside the listed words");
	return 0;
}

static uint32_t lcg = 0x12345678u;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg; }

/* the after-ABL state (observed oriole-peri1): amp pads func 0 dat 0 pud 1 drv 0, mode 0, gates open; other pins random */
static void reset_hw(void)
{
	memset(&h, 0, sizeof(h));
	memset(amp, 0, sizeof(amp));
	memset(first_frame_tick, 0, sizeof(first_frame_tick));
	h.sys = 0x00000000u; h.mux = 0x10u; h.div = 0; h.ipclk = 0x00200000u; h.pclk = 0x00200000u; h.qch = 0x003F0002u;
	for (int i = 0; i < NB; i++) { h.con[i] = rnd(); h.dat[i] = rnd() & 0xffu; h.pud[i] = rnd(); h.drv[i] = rnd(); }
	h.con[0] &= ~0xffffu; h.dat[0] &= ~0xfu; h.drv[0] &= ~0xffu;		/* gpp25-0..3 func 0 dat 0 drv 0 */
	h.pud[0] = (h.pud[0] & ~0xffffu) | 0x1111u;
	h.con[1] &= ~0xfu; h.dat[1] &= ~1u; h.pud[1] = (h.pud[1] & ~0xfu) | 1u;	/* gpp1-0 */
	h.con[2] &= ~0xf000u; h.dat[2] &= ~8u;					/* gpp16-3 */
	h.con[3] &= ~0xf0000000u; h.dat[3] &= ~0x80u;				/* gpp23-7 */
	h.usicon = 1u; h.ch = 0; h.clk = 0; h.mode = 0; h.slave = 1u; h.inten = 0; h.pkt = 0; h.swap = 0; h.fb = 0; h.usiopt = 0;
	h.ch |= ch_snap_inject;
	for (int a = 0; a < 2; a++) { amp[a].boot_polls = 3; amp[a].mask1 = 0xFFFFFFFFu; amp[a].ccm = 0x1u; }
	nw = nreads = nframes = violations = mux_writes = 0; why_v[0] = 0;
	rfault_pa = 0; rfault_after = 0; rfault_cnt = 0; wfault_n = 0; ignore_from = 0; no_clock = 0;
	ticks = 1000; tick_step = 1; freq = 1000000;		/* 1 MHz counter: 1 ms = 1000 ticks */
	nl = 0;
	h0 = h;
}

static int has(const char *s)
{
	for (unsigned i = 0; i < nl; i++)
		if (strstr(lines[i], s))
			return 1;
	return 0;
}

static void dump(void)
{
	for (unsigned i = 0; i < nl; i++)
		printf("  | %s\n", lines[i]);
}

/* every word the probe may touch is back at its pre-value (the bus side effects aside); resets low */
static int restored_exact(void)
{
	for (int i = 0; i < 4; i++)
		if (h.con[i] != h0.con[i] || h.dat[i] != h0.dat[i] || h.pud[i] != h0.pud[i] || h.drv[i] != h0.drv[i])
			return 0;
	return h.sys == h0.sys && h.mux == h0.mux && h.usicon == h0.usicon && h.ch == h0.ch && h.mode == h0.mode && h.slave == h0.slave &&
	       h.inten == h0.inten && h.pkt == h0.pkt && h.swap == h0.swap && h.fb == h0.fb && !released(0) && !released(1);
}

static unsigned fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s (%s)\n", what, #c); dump(); fails++; } } while (0)

static void full_ok(const char *label, uint32_t mux0)
{
	int rc;

	reset_hw();
	h.mux = mux0; h0 = h;
	rc = amp_init_run();
	CHECK(rc == 0, label);
	CHECK(violations == 0, why_v);
	CHECK(has("SWEET AMP RESULT: l=0x35a40/0xb2 r=0x35a40/0xb2 restore=ok"), label);
	CHECK(has("SWEET AMP: init ok l,r devid 35a40 rev b2, errors=0"), label);
	CHECK(amp[0].nwr == 12 && amp[1].nwr == 12 && !amp[0].seq_bad && !amp[1].seq_bad, "each amp got exactly the 12 stock writes in order");
	CHECK(restored_exact(), label);
	CHECK(amp[0].frames >= 4 && amp[1].frames >= 4, "both amps answered at least four frames");
	CHECK(nframes <= 2u * (100u + 4u + 12u + 3u), "no more frames than polls + reads + the init per amp");
	if (!(mux0 & 0x10u)) {
		/* no mux write at all: run again counting writes to MUX via the violation-free path */
		CHECK(h.mux == mux0 && mux_writes == 0, "mux never written");
	}
	printf("ok: %s (%u writes, %u reads, %u frames)\n", label, nw, nreads, nframes);
}

/* a refusal: no write at all, the refused line, the result line */
static void refused(const char *label, void (*setup)(void), const char *need)
{
	int rc;

	reset_hw();
	setup();
	h0 = h;
	rc = amp_init_run();
	CHECK(rc == 1, label);
	CHECK(nw == 0, label);
	CHECK(violations == 0, why_v);
	CHECK(has("AMP refused: "), label);
	CHECK(has(need), label);
	CHECK(has("SWEET AMP RESULT: l=none r=none restore=n/a"), label);
	printf("ok: refused, zero writes: %s\n", label);
}

static void s_nofreq(void) { freq = 0; }
static void s_dead(void) { tick_step = 0; }
static void s_sysfault(void) { rfault_pa = SYSREG; }
static void s_gate(void) { h.ipclk = 0; }
static void s_pclk(void) { h.pclk = 0; }
static void s_qch(void) { h.qch = 0; }
static void s_i2c(void) { h.sys = 0x4u; }
static void s_busy(void) { h.mux_busy = 100; }
static void s_padfault(void) { rfault_pa = G23 + 4; }
static void s_spifunc(void) { h.con[0] |= 0x3u; }
static void s_csfunc(void) { h.con[1] |= 0x2u; }
static void s_rsthigh(void) { h.con[2] |= 0x1000u; h.dat[2] |= 8u; }
static void s_rstfunc(void) { h.con[3] |= 0x20000000u; }

/* a stop after writes: exact restore, no violation, result line not ok */
static void stopped(const char *label, int want_restore_ok)
{
	int rc = amp_init_run();

	CHECK(rc == 1, label);
	CHECK(violations == 0, why_v);
	CHECK(!has("restore=ok") || want_restore_ok, label);
	if (want_restore_ok) {
		CHECK(restored_exact(), label);
		CHECK(has("restore=ok"), label);
	}
	printf("ok: stopped and restored: %s (%u writes)\n", label, nw);
}

static void unit_writes(void)
{
	/* TX bytes of a write frame only while an allowed pair is armed, each at its position; never PWR_CTRL1 */
	CHECK(!w32_allowed(R_TX, 0u << 6, 0x00u, 0, 0, 0, 0, 0), "write frame byte 0 without an armed write refused");
	CHECK(w32_allowed(R_TX, 0u << 6, 0x00u, 0, 0, 1, 0x10110u, 0x7FFCFE3Fu), "armed IRQ1_MASK1 byte 0");
	CHECK(w32_allowed(R_TX, 2u << 6, 0x01u, 0, 0, 1, 0x10110u, 0x7FFCFE3Fu) && w32_allowed(R_TX, 3u << 6, 0x10u, 0, 0, 1, 0x10110u, 0x7FFCFE3Fu), "armed address bytes");
	CHECK(w32_allowed(R_TX, 6u << 6, 0x7Fu, 0, 0, 1, 0x10110u, 0x7FFCFE3Fu) && w32_allowed(R_TX, 9u << 6, 0x3Fu, 0, 0, 1, 0x10110u, 0x7FFCFE3Fu), "armed value bytes");
	CHECK(!w32_allowed(R_TX, 9u << 6, 0x3Eu, 0, 0, 1, 0x10110u, 0x7FFCFE3Fu), "another value byte refused");
	CHECK(!w32_allowed(R_TX, 10u << 6, 0x00u, 0, 0, 1, 0x10110u, 0x7FFCFE3Fu), "no eleventh byte");
	CHECK(!w32_allowed(R_TX, 0u << 6, 0x00u, 0, 0, 1, 0x2014u, 1u), "PWR_CTRL1 armed: refused");
	CHECK(!w32_allowed(R_TX, 3u << 6, 0x14u, 0, 0, 1, 0x2014u, 1u), "PWR_CTRL1 armed: refused (address byte)");
	CHECK(!w32_allowed(R_TX, 0u << 6, 0x00u, 0, 0, 1, 0x10110u, 0u), "IRQ1_MASK1 with another value: refused");
	CHECK(!w32_allowed(R_TX, 0u << 6, 0x00u, 0, 0, 1, 0x4314u, 0u), "a register outside the sequence: refused");
	CHECK(!w32_allowed(R_TX, 0u << 6, 0x00u, CH_TX, 0, 1, 0x10110u, 0x7FFCFE3Fu), "no armed byte with the transmitter on");
	CHECK(w32_allowed(R_CH, 0, CH_TX | CH_RX, 10u << 6, 0, 1, 0x40u, 0x5555u), "TX on with a full armed write frame");
	CHECK(!w32_allowed(R_CH, 0, CH_TX | CH_RX, 10u << 6, 0, 0, 0, 0), "TX on with 10 bytes and nothing armed: refused");
	CHECK(!w32_allowed(R_CH, 0, CH_TX | CH_RX, 6u << 6, 0, 1, 0x40u, 0x5555u), "TX on with a partial write frame: refused");
	CHECK(!w32_allowed(R_CH, 0, CH_TX | CH_RX, 10u << 6, 0, 1, 0x2014u, 1u), "TX on for PWR_CTRL1: refused");
	CHECK(w32_allowed(R_PKT, 0, 10u, 0, 0, 1, 0x40u, 0x5555u) && !w32_allowed(R_PKT, 0, 10u, 0, 0, 0, 0, 0), "PACKET_CNT 10 only when armed");
	CHECK(amp_write_ok(0x2BC1000u, 0u) && !amp_write_ok(0x2014u, 0u) && !amp_write_ok(0x2014u, 1u) && !amp_write_ok(0x2BC1000u, 1u), "amp_write_ok");
	printf("ok: armed write unit cases\n");
}

static void unit_allowlist(void)
{
	struct amp_snap s0;
	uint32_t rv = 0x12345678u;

	memset(&s0, 0, sizeof(s0));
	/* registers that are never writable */
	CHECK(!amp_reg_writable(R_CLK), "CLK_CFG not writable");
	CHECK(!amp_reg_writable(R_STATUS), "STATUS not writable");
	CHECK(!amp_reg_writable(R_RX), "RX_DATA not writable");
	CHECK(!amp_reg_writable(R_USIOPT), "USI_OPTION not writable");
	CHECK(!amp_reg_writable(DIV), "the CMU divider not writable");
	CHECK(!amp_reg_writable(IPCLK) && !amp_reg_writable(QCH), "CMU gates not writable");
	for (int i = 0; i < NB; i++)
		CHECK(!amp_reg_writable(BANKS[i] + 8), "no PUD register writable");
	CHECK(!amp_reg_writable(GA6) && !amp_reg_writable(GA9 + 4), "IRQ pads not writable");
	CHECK(!amp_reg_writable(G1 + 0xC) && !amp_reg_writable(G16 + 0xC) && !amp_reg_writable(G23 + 0xC), "DRV only on gpp25");
	CHECK(!amp_reg_writable(0x10C21010UL), "the USI12 (I2C) mode word not writable");
	CHECK(!amp_reg_writable(0x10D50000UL) && !amp_reg_writable(0x10D40000UL), "other USI blocks not writable");
	CHECK(amp_reg_writable(R_TX) && amp_reg_writable(R_CH) && amp_reg_writable(SYSREG) && amp_reg_writable(MUX), "the listed words");
	/* TX bytes: position = TX FIFO level (STATUS[12:6]); transmitter off (aux = CH_CFG) */
	CHECK(WA(R_TX, 0u << 6, 0x80u, 0, 0), "first byte 0x80");
	CHECK(!WA(R_TX, 0u << 6, 0x00u, 0, 0), "first byte 0x00 (a write frame) refused");
	CHECK(!WA(R_TX, 0u << 6, 0x81u, 0, 0), "first byte 0x81 refused");
	CHECK(!WA(R_TX, 0u << 6, 0x20u, 0, 0), "first byte 0x20 (PWR_CTRL1 high byte) refused");
	CHECK(WA(R_TX, 1u << 6, 0x01u, 0, 0) && WA(R_TX, 1u << 6, 0x00u, 0, 0), "second byte 0x01 / 0x00");
	CHECK(!WA(R_TX, 1u << 6, 0x80u, 0, 0), "second byte 0x80 refused");
	CHECK(WA(R_TX, 3u << 6, 0x1Cu, 0, 0) && WA(R_TX, 3u << 6, 0x18u, 0, 0) && WA(R_TX, 3u << 6, 0x04u, 0, 0), "fourth byte");
	CHECK(!WA(R_TX, 3u << 6, 0x14u, 0, 0), "fourth byte 0x14 (PWR_CTRL1 low byte) refused");
	CHECK(!WA(R_TX, 2u << 6, 0x20u, 0, 0), "third byte 0x20 refused");
	CHECK(!WA(R_TX, 4u << 6, 0x01u, 0, 0) && !WA(R_TX, 5u << 6, 0xffu, 0, 0), "pad bytes are 0");
	CHECK(!WA(R_TX, 6u << 6, 0x00u, 0, 0), "no seventh byte");
	CHECK(!WA(R_TX, 0u << 6, 0x80u, CH_TX, 0), "no TX byte while the transmitter is on");
	CHECK(!WA(R_TX, 0u << 6, 0x180u, 0, 0), "no wide TX write");
	/* CH_CFG */
	CHECK(WA(R_CH, 0, CH_TX | CH_RX, 6u << 6, 0), "TX on with one full frame queued");
	CHECK(!WA(R_CH, 0, CH_TX | CH_RX, 5u << 6, 0), "TX on with 5 bytes refused");
	CHECK(!WA(R_CH, 0, CH_TX | CH_RX, 0u << 6, 0), "TX on with an empty FIFO refused (underrun = undefined bytes)");
	CHECK(!WA(R_CH, 0, CH_TX | CH_RX | CH_CPHA, 6u << 6, 0), "TX on in mode 1 refused");
	CHECK(!WA(R_CH, 0, CH_TX | CH_RX | CH_CPOL, 6u << 6, 0), "TX on in mode 2 refused");
	CHECK(!WA(R_CH, 0, CH_TX | CH_RX | CH_SLAVE, 6u << 6, 0), "TX on as slave refused");
	CHECK(!WA(R_CH, 0, CH_TX | CH_RX | 0x40u, 6u << 6, 0), "TX on with HS_EN refused");
	CHECK(!WA(R_CH, 0, CH_TX, 6u << 6, 0), "TX on without RX refused");
	CHECK(!WA(R_CH, CH_TX, CH_TX | CH_RX, 6u << 6, 0), "TX on again while on refused");
	CHECK(WA(R_CH, 0, CH_RX, 0, 0) && WA(R_CH, 0, CH_SWRST, 0, 0) && WA(R_CH, 0, 0, 0, 0), "RX only / SW_RST / off");
	CHECK(!WA(R_CH, 0, CH_CPHA, 0, 0), "CPHA refused");
	CHECK(WA(R_CH, 0, CH_CPOL, 0, CH_CPOL), "the snapshot (channels off) as a restore value");
	CHECK(!WA(R_CH, 0, CH_CPOL | CH_TX, 6u << 6, CH_CPOL | CH_TX), "no snapshot with a channel on");
	/* the other controller words */
	CHECK(WA(R_MODE, 0, 0x1FF80000u, 0, 0) && WA(R_MODE, 0, 0, 0, 0) && !WA(R_MODE, 0, 0x1FF80003u, 0, 0), "MODE_CFG: 0, trailing count");
	CHECK(!WA(R_MODE, 0, 0x8u, 0, 0), "MODE_CFG loopback refused");
	CHECK(WA(R_SLAVE, 0, 0x1u, 0, 0) && WA(R_SLAVE, 0, 0x23u, 0, 0) && !WA(R_SLAVE, 0, 0x0u, 0, 1), "SLAVE_SEL");
	CHECK(WA(R_PKT, 0, 6u, 0, 0) && WA(R_PKT, 0, 0x10004u, 0, 0) && !WA(R_PKT, 0, 0x10007u, 0, 0) && !WA(R_PKT, 0, 0x1000Au, 0, 0), "PACKET_CNT");
	CHECK(!WA(R_PKT, 0, 0x10010u, 0, 0x10010u), "no enabled snapshot");
	CHECK(WA(R_PND, 0, 0x1eu, 0, 0) && WA(R_PND, 0, 0, 0, 0) && !WA(R_PND, 0, 0x1u, 0, 0), "PENDING_CLR");
	CHECK(WA(R_FB, 0, 1u, 0, 0) && !WA(R_FB, 0, 2u, 0, 0) && WA(R_FB, 0, 3u, 0, 3u), "FB_CLK");
	CHECK(WA(R_USICON, 1, 0, 0, 1) && WA(R_USICON, 0, 1, 0, 1) && !WA(R_USICON, 0, 2, 0, 1), "USI_CON");
	CHECK(WA(R_INTEN, 0, 0, 0, 5) && !WA(R_INTEN, 0, 1, 0, 5), "INT_EN");
	/* sysreg mode and mux */
	CHECK(WA(SYSREG, 0xF0u, 0xF2u, 0, 0xF0u) && !WA(SYSREG, 0xF0u, 0xF4u, 0, 0xF0u) && !WA(SYSREG, 0xF0u, 0x02u, 0, 0xF0u), "mode field only, SPI");
	CHECK(WA(SYSREG, 0xF2u, 0xF0u, 0, 0xF0u), "mode restore");
	CHECK(WA(MUX, 0x10u, 0x00u, 0, 0x10u) && WA(MUX, 0x00u, 0x10u, 0, 0x10u) && !WA(MUX, 0x00u, 0x10u, 0, 0x00u), "mux: osc, or back to the snapshot");
	CHECK(!WA(MUX, 0x10u, 0x01u, 0, 0x10u), "mux: no other bit");
	/* pads */
	CHECK(WA(G25, 0x0000u, 0x1222u, 0, 0), "gpp25 CON: SPI func 2, CS0 output");
	CHECK(!WA(G25, 0x0000u, 0x1322u, 0, 0), "gpp25 CON: MISO func 3 refused");
	CHECK(!WA(G25, 0x0000u, 0x2222u, 0, 0), "gpp25 CON: CS0 func 2 (the NSS) refused");
	CHECK(!WA(G25, 0x00000u, 0x10000u, 0, 0), "gpp25 CON: pin 4 refused");
	CHECK(WA(G25 + 4, 0x0u, 0x8u, 0, 0) && !WA(G25 + 4, 0x0u, 0x1u, 0, 0), "gpp25 DAT: CS0 only");
	CHECK(WA(G25 + 0xC, 0x0u, 0x11u, 0, 0) && !WA(G25 + 0xC, 0x0u, 0x111u, 0, 0) && !WA(G25 + 0xC, 0x0u, 0x33u, 0, 0), "gpp25 DRV: CLK/MOSI 1");
	CHECK(WA(G16, 0x0u, 0x1000u, 0, 0) && !WA(G16, 0x0u, 0x0100u, 0, 0) && !WA(G16, 0x0u, 0x2000u, 0, 0), "gpp16 CON: RST_L output only");
	CHECK(WA(G16 + 4, 0x0u, 0x8u, 0, 0) && !WA(G16 + 4, 0x0u, 0x4u, 0, 0), "gpp16 DAT: not BT_REG_ON");
	CHECK(WA(G23 + 4, 0x30u, 0xB0u, 0, 0) && !WA(G23 + 4, 0x30u, 0x80u, 0, 0), "gpp23 DAT: not the I2C12 pins");
	CHECK(WA(G23, 0x00220000u, 0x10220000u, 0, 0) && !WA(G23, 0x00220000u, 0x10000000u, 0, 0), "gpp23 CON: RST_R only");
	CHECK(WA(G1, 0x5u, 0x0u, 0, 0x0u) && WA(G1, 0x0u, 0x1u, 0, 0x0u) && !WA(G1, 0x0u, 0x2u, 0, 0x0u), "gpp1 CON: CS1 out, or the snapshot");
	CHECK(!WA(GA6, 0, 0, 0, 0), "IRQ pad refused");
	(void)rv;
	(void)s0;
	printf("ok: allowlist unit cases\n");
}

int main(void)
{
	int rc;

	unit_allowlist();
	unit_writes();
	full_ok("full run, mux on the TOP source (switched to oscclk and back)", 0x10u);
	full_ok("full run, mux already on oscclk (never written)", 0x00u);

	refused("no timer", s_nofreq, "no timer");
	refused("counter not running", s_dead, "counter not running");
	refused("sysreg unreadable", s_sysfault, "unreadable");
	refused("IPCLK gated", s_gate, "ipclk-gated");
	refused("PCLK gated", s_pclk, "pclk-gated");
	refused("QCH no clock request", s_qch, "qch-no-clkreq");
	refused("USI13 in I2C mode", s_i2c, "another protocol");
	refused("mux busy", s_busy, "busy");
	refused("pad word unreadable", s_padfault, "unreadable");
	refused("SPI pad in another function", s_spifunc, "SPI13 pads");
	refused("CS pad in another function", s_csfunc, "chip-select");
	refused("reset already high", s_rsthigh, "reset");
	refused("reset pad in another function", s_rstfunc, "reset");

	/* stops after writes */
	reset_hw(); ch_snap_inject = CH_RX; reset_hw(); ch_snap_inject = 0; stopped("CH_CFG has a channel on after the mode write", 1);
	CHECK(has("controller not idle"), "the reason");
	reset_hw(); rfault_pa = R_CH; stopped("controller unreadable after the mode write", 1);
	reset_hw(); amp[0].absent = 1; amp[1].absent = 1; stopped("no amp answers (MISO 0)", 1);
	CHECK(has("SWEET AMP RESULT: l=0x0/0x0 r=0x0/0x0 restore=ok"), "zeros reported");
	CHECK(has("twice"), "zero twice reported");
	reset_hw(); amp[0].ff = 1; amp[0].absent = 1; stopped("L MISO stuck high", 1);
	CHECK(has("l=0xffffffff/0xffffffff"), "ones reported");
	reset_hw(); amp[1].boot_polls = 1000; stopped("R never OTP_BOOT_DONE", 1);
	CHECK(has("AMP R otp=0"), "otp timeout reported");
	reset_hw(); amp[1].st1 = 0x40u; stopped("R reports BST_OVP_ERR after the init", 1);
	CHECK(has("!!! amp error bit") && has("SWEET AMP: init FAILED"), "error bit reported, no pass");
	reset_hw(); amp[0].absent = 1; stopped("L absent: no write frame to it", 1);
	CHECK(amp[0].nwr == 0, "no write to an unidentified amp");
	reset_hw(); no_clock = 1; stopped("the bus never clocks (RX timeout)", 1);
	CHECK(has("timeout"), "timeout reported");
	reset_hw(); (void)amp_init_run(); unsigned nfull = nw;
	for (unsigned k = 1; k <= nfull; k++) {	/* every write position of a full run, the restore included */
		reset_hw();
		wfault_n = k;
		rc = amp_init_run();
		CHECK(rc == 1, "a faulting write is a failure");
		CHECK(violations == 0, why_v);
		CHECK(!has("restore=ok") || restored_exact(), "restore=ok only when exact");
	}
	printf("ok: a faulting write at each write position of a full run stops, restores, never claims a false ok\n");
	for (unsigned k = 1; k <= 200; k += 7) {
		reset_hw();
		ignore_from = k;
		rc = amp_init_run();
		CHECK(rc == 1, "dropped writes cannot pass");
		CHECK(!has("restore=ok") || restored_exact(), "dropped writes: restore=ok only when exact");
	}
	printf("ok: dropped writes from any position never pass\n");
	reset_hw(); ignore_from = 1u << 30; rc = amp_init_run(); (void)rc;
	{
		/* a restore whose writes are dropped is loud */
		unsigned total = nw;

		reset_hw();
		ignore_from = total - 20u;
		rc = amp_init_run();
		CHECK(rc == 1, "restore dropped");
		CHECK(has("!!! RESTORE FAILED"), "loud restore failure");
		CHECK(has("restore=FAILED"), "restore=FAILED");
		printf("ok: a dropped restore write is loud (!!! RESTORE FAILED, restore=FAILED)\n");
	}
	if (fails) {
		printf("amp_xnu_host_test: %u FAILED\n", fails);
		return 1;
	}
	printf("PASS amp_xnu_host_test\n");
	return 0;
}

