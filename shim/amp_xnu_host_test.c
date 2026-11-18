/* Host test for xnu patch 0373 (bsd/dev/sweet_amp.h, boot-arg sweetamp=1): the A1 identify of shim/ampid_host_test.c (same fake SoC and fake CS35L41,
 * typed from the driver text) plus the A7 checks: every write frame is one of the 12 stock B2 probe writes in stock order (cs35l41.c:4306-4308,
 * :3675-3686, :4401), only after that amp was identified, never PWR_CTRL1; an error bit in IRQ1_STATUS1 fails the init; the armed-write allowlist.
 * A7 OTP: each amp unpacks its OTP words into the 38 row registers exactly as a re-typed
 * Linux v6.18 cs35l41_otp_unpack (ref_unpack, GPL-2.0 logic re-typed for the test, THIRD_PARTY row 22), only while unlocked, always relocked or loud.
 * The headers (sweet_amp.h, sweet_amp_otp.h) are extracted from the patch by shim/build.sh:
 *   gcc -O1 -Wall -Wextra -Werror -Wno-unused-function -I$O -o $O/amp_xnu_host_test amp_xnu_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
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
	uint32_t revid;		/* 0 = B2 (0xB2) */
	uint32_t devid;		/* 0 = CS35L41 (0x35A40) */
	uint32_t st1_late; unsigned st1_late_at;	/* IRQ1_STATUS1 |= st1_late once write #st1_late_at is done */
	int ignore_mask;	/* the IRQ1_MASK1 write does not stick */
	int drop_cs1_after_st1;	/* right after the pre-write IRQ1_STATUS1 read (L), CS1 is driven low (stuck) */
	int st1_seen;
	uint32_t st3;	/* IRQ1_STATUS3 (OTP_BOOT_ERR bit 31) */
	int booted;		/* OTP done after boot_polls status reads since the reset release */
	unsigned boot_polls, reads_since;
	int absent;		/* no MISO answer: reads give 0 */
	int ff;			/* MISO stuck high */
	uint8_t frame[16]; unsigned nf;
	uint64_t t_low, t_high;	/* tick of the last reset assert (output low) / release */
	int was_released;
	unsigned frames;
	/* A7 OTP: OTPID, the 32 OTP words, a register file of the OTP row registers, the test-key state (0 locked, 1 half unlocked, 2 unlocked, 3 half locked) */
	uint32_t otpid, mem[32], ra[48], rv[48], ra0[48], rv0[48]; unsigned nreg, nreg0, mem_reads, otp_nwr, lock_seen;
	int key, mem_flip;	/* mem_flip: OTP_MEM5 reads differently from the second pass on */
	uint32_t stuck_reg;	/* writes to this register do not stick */
	/* patch 0378 A9: the TDM / boost / clock registers, PWR_CTRL1 (read only), the PLL model */
	uint32_t a9v[11]; unsigned a9_nwr, a9_bad; int pll_en_seen;
	/* patch 0381 A9e: the DSP (XM / YM 24-bit words, PM raw registers), its control words, the stream position of a burst */
	uint32_t xm[4096], ym[2048], pm[0x1400], rxsrc[4], mpu[20], wdt, errata, wacc, ymp_first, ymu_first;
	unsigned np, dsp_mem_nw, dsp_ctl_nw, starts, stops; int mpulock, reset_running;
};
static struct amp amp[2];
static unsigned violations;
static const uint32_t SEQ[12][2] = { { 0x10110u, 0x7FFCFE3Fu }, { 0x40u, 0x5555u }, { 0x40u, 0xAAAAu }, { 0x4100u, 0u }, { 0x4310u, 0u }, { 0x4400u, 0u },
				     { 0x381Cu, 0x51u }, { 0x2BC20E0u, 0u }, { 0x2BC2020u, 0u }, { 0x40u, 0xCCCCu }, { 0x40u, 0x3333u }, { 0x2BC1000u, 0u } };
static char why_v[256];
static unsigned nw, nreads, nframes;
static unsigned mark_unlock[2], mark_lock[2], mark_otp_end[2];	/* nw at the unlock (key 2), the first lock word, the second lock word */
static uintptr_t rfault_pa;
static unsigned rfault_after;		/* the fault applies only after this many reads of rfault_pa */
static unsigned rfault_cnt;
static unsigned wfault_n;		/* 1-based write index that faults (not applied) */
static uintptr_t wfault_pa;		/* patch 0383: the address of the faulted write */
static unsigned ignore_from;		/* writes from this 1-based index on are dropped (accepted) */
static int no_clock;			/* the bus never clocks (RX never fills) */
static uint64_t ticks, tick_step, freq;
static uint64_t first_frame_tick[2];
static char lines[512][RAMLOG_LINE_MAX + 8];
static unsigned nl;
static uint32_t ch_snap_inject;
static unsigned flip_at; static int flip_bank; static uint32_t flip_bit;	/* at write #flip_at, DAT of flip_bank toggles flip_bit (another pin's input level) */
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
/* ---- the reference OTP unpack: Linux v6.18 cs35l41_otp_unpack (cs35l41-lib.c:813-880) typed again, int offsets and GENMASK as there; the
 * map rows are the header tables (data) ---- */
#define GENMASK(h, l)	((~0u << (l)) & (~0u >> (31 - (h))))
static const struct amp_otp_field *const REFMAPS[2] = { amp_otp_map_1, amp_otp_map_2 };
static int ref_target(uint32_t reg)
{
	for (int m = 0; m < 2; m++)
		for (int i = 0; i < 99; i++)
			if (reg != 0 && REFMAPS[m][i].reg == reg)
				return 1;
	return 0;
}
static uint32_t ref_mask(uint32_t reg)
{
	uint32_t k = 0;

	for (int m = 0; m < 2; m++)
		for (int i = 0; i < 99; i++)
			if (REFMAPS[m][i].reg == reg)
				k |= GENMASK(REFMAPS[m][i].shift + REFMAPS[m][i].size - 1, REFMAPS[m][i].shift);
	return k;
}
static uint32_t *regp(uint32_t *ra, uint32_t *rv, unsigned *n, uint32_t reg)
{
	for (unsigned i = 0; i < *n; i++)
		if (ra[i] == reg)
			return &rv[i];
	assert(*n < 48);
	ra[*n] = reg;
	rv[*n] = 0;
	return &rv[(*n)++];
}
/* apply the Linux unpack for otp id `id` to the register file (ra, rv, n); 0 = done, -1 = no map */
static int ref_unpack(uint32_t id, const uint32_t *otp_mem, uint32_t *ra, uint32_t *rv, unsigned *n)
{
	const struct amp_otp_field *otp_map;
	int bit_offset = 16, word_offset = 2, i;	/* every entry of cs35l41_otp_map_map: 16, 2 */
	uint32_t otp_val;

	if (id == 1 || id == 8) otp_map = amp_otp_map_1;
	else if (id == 2 || id == 3 || id == 6) otp_map = amp_otp_map_2;
	else return -1;
	for (i = 0; i < 99; i++) {
		if (bit_offset + otp_map[i].size - 1 >= 32) {
			otp_val = (otp_mem[word_offset] & GENMASK(31, bit_offset)) >> bit_offset;
			otp_val |= (otp_mem[++word_offset] & GENMASK(bit_offset + otp_map[i].size - 33, 0)) << (32 - bit_offset);
			bit_offset += otp_map[i].size - 32;
		} else {
			otp_val = (otp_mem[word_offset] & GENMASK(bit_offset + otp_map[i].size - 1, bit_offset)) >> bit_offset;
			bit_offset += otp_map[i].size;
		}
		if (bit_offset == 32) {
			bit_offset = 0;
			word_offset++;
		}
		if (otp_map[i].reg != 0) {
			uint32_t mask = GENMASK(otp_map[i].shift + otp_map[i].size - 1, otp_map[i].shift), *r = regp(ra, rv, n, otp_map[i].reg);

			*r = (*r & ~mask) | ((otp_val << otp_map[i].shift) & mask);	/* regmap_update_bits */
		}
	}
	return 0;
}

/* ---- patch 0378 A9 model: registers typed from cs35l41.h (not from the header under test) ---- */
static const uint32_t A9R[11] = { 0x3810u, 0x3814u, 0x3808u, 0x480Cu, 0x6808u, 0x4808u, 0x2D10u, 0x2C04u, 0x2C0Cu, 0x4840u, 0x2014u };
static int sclk_on, pll_never, a9_opens, a9_closes, a9_open_ret = 1;
static uint32_t a9v0[2][11];
static int a9_idx(uint32_t reg)
{
	for (int i = 0; i < 11; i++)
		if (A9R[i] == reg)
			return i;
	return -1;
}
/* the expected final word of each A9 register from its start value: the stock fields (cs35l41.c set_pdata / set_dai_fmt / set_sysclk / hw_params) */
static uint32_t a9_expect(uint32_t reg, uint32_t v0)
{
	switch (reg) {
	case 0x3810u: return (v0 & ~0xFFFFu) | 0x4932u;
	case 0x3814u: return (v0 & ~0xFF03u) | 0x7500u;
	case 0x3808u: return (v0 & ~0x7Fu) | 0x36u;
	case 0x480Cu: return v0 | 3u;
	case 0x6808u: return v0 | 0x3F00u;
	case 0x4808u: return (v0 & ~(0xFF000000u | 0x700u | 0x10u | 0x1u | 0x4u | 0x40u)) | 0x10000000u;
	case 0x2D10u: return 0x18010u;
	case 0x2C04u: return (v0 & ~(0x800u | 0x7E0u | 0x10u | 0x7u)) | 0x500u | 0x10u;
	case 0x2C0Cu: return (v0 & ~0x1Fu) | 3u;
	case 0x4840u: return (v0 & ~0x3Fu) | 16u;
	default: return v0;
	}
}
static void a9_wr(int a, uint32_t wa, uint32_t wv)
{
	struct amp *m = &amp[a];
	int i = a9_idx(wa);

	if (wa == 0x10018u) {		/* IRQ1_STATUS3 write-1-to-clear */
		if (wv != 0x2u)
			violate("an IRQ1_STATUS3 write other than PLL_LOCK");
		m->st3 &= ~wv;
		m->a9_nwr++;
		return;
	}
	if (i < 0 || wa == 0x2014u) {
		violate(wa == 0x2014u ? "PWR_CTRL1 written by A9" : "an A9-phase write to a register outside the A9 set");
		m->a9_bad++;
		return;
	}
	m->a9v[i] = wv;
	m->a9_nwr++;
	if (wa == 0x2C04u && (wv & 0x10u))
		m->pll_en_seen = 1;
}
/* IRQ1_STATUS3 as read: PLL_LOCK latches once the PLL is enabled from SCLK (sel 0, closed loop) and the AOC drives SCLK */
static uint32_t a9_st3(int a)
{
	struct amp *m = &amp[a];
	uint32_t pll = m->a9v[7];

	if (!pll_never && sclk_on && (pll & 0x10u) && !(pll & 0x800u) && (pll & 7u) == 0u && ((pll >> 5) & 0x3Fu) == 0x28u)
		m->st3 |= 0x2u;
	return m->st3;
}
static int cb_open(void *ctx) { (void)ctx; a9_opens++; sclk_on = a9_open_ret; return a9_open_ret; }
/* patch 0379 (sweetaoc=5) model: the open starts the stream but SCLK only runs after tick_need ticks (0 = never); a tick outside open..close is a violation */
static int a9_ticks, tick_need, tick_bad;
static int cb_open5(void *ctx) { (void)ctx; a9_opens++; a9_ticks = 0; sclk_on = 0; return a9_open_ret; }
static void cb_tick(void *ctx) { (void)ctx; if (a9_opens != 1 || a9_closes) tick_bad++; a9_ticks++; sclk_on = tick_need && a9_ticks >= tick_need; }
static void cb_close(void *ctx) { (void)ctx; a9_closes++; sclk_on = 0; }

/* an A7 write after the 12 init writes: the test-key order, OTP row registers only while unlocked, only bits of its rows */
static void otp_wr(int a, uint32_t wa, uint32_t wv)
{
	struct amp *m = &amp[a];

	m->otp_nwr++;
	if (wa == 0x40u) {
		if (wv == 0x55u && m->key == 0) m->key = 1;
		else if (wv == 0xAAu && m->key == 1) { m->key = 2; mark_unlock[a] = nw; }
		else if (wv == 0xCCu) { m->key = 3; mark_lock[a] = nw; }
		else if (wv == 0x33u && m->key == 3) { m->key = 0; m->lock_seen++; mark_otp_end[a] = nw; }
		else if (wv == 0x33u) { }	/* its 0xCC was lost (a faulted frame): the key stays as it is, the run must say lock=FAILED */
		else violate("a TEST_KEY_CTL write out of the unlock / lock order");
		return;
	}
	if (amp_a9_cb.on && m->lock_seen && (a9_idx(wa) >= 0 || wa == 0x10018u)) {
		a9_wr(a, wa, wv);	/* patch 0378: after the OTP relock */
		return;
	}
	if (!ref_target(wa)) {
		violate("a write after the init that is not an OTP row register");
		return;
	}
	if (m->key != 2)
		violate("an OTP row register written while the test key is locked");
	{
		uint32_t *r = regp(m->ra, m->rv, &m->nreg, wa);

		if ((*r ^ wv) & ~ref_mask(wa))
			violate("an OTP write changes bits outside the rows of its register");
		if (wa != m->stuck_reg)
			*r = wv;
	}
}

/* the amp's row registers equal the Linux unpack applied to its starting register file */
static int otp_as_linux(int a)
{
	uint32_t ra[48], rv[48];
	unsigned n = amp[a].nreg0;

	memcpy(ra, amp[a].ra0, sizeof(ra));
	memcpy(rv, amp[a].rv0, sizeof(rv));
	if (ref_unpack(amp[a].otpid, amp[a].mem, ra, rv, &n))
		return 0;
	for (unsigned i = 0; i < n; i++)
		if (*regp(amp[a].ra, amp[a].rv, &amp[a].nreg, ra[i]) != rv[i])
			return 0;
	return n == amp[a].nreg;
}

/* ---- patch 0381 A9e: the DSP model, typed from Linux v6.18 include/sound/cs35l41.h and the AMP wm_adsp.c HALO offsets (not from the header):
 * XM / YM as 24-bit words behind a packed view (4 words in 3 registers, LSB first: word 0 = bits 0..23 of the first register) and an unpacked
 * view (one word per register), PM as raw registers. The core runs once CCM_CORE_CTRL EN is set after a full load; the heartbeat word (fw
 * xm_base + 1) counts each read while it runs. ---- */
static int hb_dead, hb_needs_sclk, dsp_err_after_start = -1;
static uint32_t dsp_stuck_addr; static int dsp_stuck_amp = -1;
static unsigned dsp_wmax, dsp_rmax;	/* patch 0382: the longest write / read burst seen (data bytes after the 6 address / pad bytes) */
/* patch 0383: the MPU access words read back through this mask (hardware that does not return all ones); nw when L's 3rd access word landed */
static uint32_t dsp_mpu_rbmask = 0xFFFFFFFFu; static unsigned mark_mpu3;
static int dsp_mem(uint32_t x)
{
	return (x >= 0x2000000u && x <= 0x2002FF0u) || (x >= 0x2800000u && x <= 0x2803FF4u) || (x >= 0x2C00000u && x <= 0x2C017F0u) ||
	       (x >= 0x3400000u && x <= 0x3401FF4u) || (x >= 0x3800000u && x <= 0x3804FE8u);
}
static int dsp_errata(uint32_t x) { return ((x >= 0x2B80080u && x <= 0x2B800B8u) || (x >= 0x2B80280u && x <= 0x2B802B8u)) && !(x & 7u); }
static int dsp_mpu(uint32_t x)
{
	static const uint32_t o[20] = { 0, 4, 8, 0xC, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x5C };
	for (int i = 0; i < 20; i++)
		if (x == 0x2BC3000u + o[i])
			return i + 1;
	return 0;
}
static int dsp_ctl(uint32_t x)	/* registers the DSP part may WRITE */
{
	return dsp_errata(x) || (x >= 0x4C50u && x <= 0x4C5Cu && !(x & 3u)) || dsp_mpu(x) || x == 0x2BC3140u || x == 0x2BC1000u || x == 0x2BC7000u;
}
static int dsp_rd(uint32_t x)	/* + read only */
{
	return dsp_ctl(x) || x == 0x13004u || x == 0x2C08u || (x >= 0x2B805C0u && x <= 0x2B805D8u && !(x & 7u)) || (x >= 0x2BC3100u && x <= 0x2BC3114u);
}
static uint32_t pk_get(const uint32_t *w, uint32_t k)
{
	unsigned __int128 b = 0;
	uint32_t g = k / 3u, j = k % 3u;

	for (int i = 3; i >= 0; i--)
		b = (b << 24) | (w[4u * g + (uint32_t)i] & 0xFFFFFFu);
	return (uint32_t)(b >> (32u * j));
}
static void pk_put(uint32_t *w, uint32_t k, uint32_t v)
{
	unsigned __int128 b = 0, m = (unsigned __int128)0xFFFFFFFFu;
	uint32_t g = k / 3u, j = k % 3u;

	for (int i = 3; i >= 0; i--)
		b = (b << 24) | (w[4u * g + (uint32_t)i] & 0xFFFFFFu);
	b = (b & ~(m << (32u * j))) | ((unsigned __int128)v << (32u * j));
	for (int i = 0; i < 4; i++)
		w[4u * g + (uint32_t)i] = (uint32_t)(b >> (24u * (uint32_t)i)) & 0xFFFFFFu;
}
static int dsp_running(int a)
{
	return (amp[a].ccm & 1u) && !hb_dead && (!hb_needs_sclk || sclk_on);
}
static uint32_t dsp_get(int a, uint32_t x)
{
	struct amp *m = &amp[a];

	if (x >= 0x2000000u && x < 0x2003000u) return pk_get(m->xm, (x - 0x2000000u) / 4u);
	if (x >= 0x2C00000u && x < 0x2C01800u) return pk_get(m->ym, (x - 0x2C00000u) / 4u);
	if (x >= 0x2800000u && x < 0x2804000u) {
		uint32_t k = (x - 0x2800000u) / 4u;

		if (k == m->xm[5] + 1u && dsp_running(a))
			m->xm[k] = (m->xm[k] + 1u) & 0xFFFFFFu;
		return m->xm[k];
	}
	if (x >= 0x3400000u && x < 0x3402000u) return m->ym[(x - 0x3400000u) / 4u];
	return m->pm[(x - 0x3800000u) / 4u];
}
static void dsp_put(int a, uint32_t x, uint32_t v)
{
	struct amp *m = &amp[a];

	if (m->ccm & 1u)
		violate("DSP memory written while the core runs");
	if (a == dsp_stuck_amp && x == dsp_stuck_addr)
		return;
	m->dsp_mem_nw++;
	if (x >= 0x2C00000u && x < 0x2C01800u && !m->ymp_first) m->ymp_first = x;
	if (x >= 0x3400000u && x < 0x3402000u && !m->ymu_first) m->ymu_first = x;
	if (x >= 0x2000000u && x < 0x2003000u) pk_put(m->xm, (x - 0x2000000u) / 4u, v);
	else if (x >= 0x2C00000u && x < 0x2C01800u) pk_put(m->ym, (x - 0x2C00000u) / 4u, v);
	else if (x >= 0x2800000u && x < 0x2804000u) m->xm[(x - 0x2800000u) / 4u] = v & 0xFFFFFFu;
	else if (x >= 0x3400000u && x < 0x3402000u) m->ym[(x - 0x3400000u) / 4u] = v & 0xFFFFFFu;
	else m->pm[(x - 0x3800000u) / 4u] = v;
}
static void dsp_ctl_wr(int a, uint32_t x, uint32_t v)
{
	struct amp *m = &amp[a];
	int i;

	m->dsp_ctl_nw++;
	if (dsp_errata(x)) {
		if (v != 1u) violate("an errata word other than 1");
		m->errata |= 1u << ((x & 0x200u ? 8u : 0u) + ((x & 0xFFu) - 0x80u) / 8u);
	} else if (x >= 0x4C50u && x <= 0x4C5Cu) {
		static const uint32_t want[4] = { 0x28u, 0x21u, 0x3Au, 0x3Bu };
		if (v != want[(x - 0x4C50u) / 4u]) violate("a DSP RX source other than stock");
		m->rxsrc[(x - 0x4C50u) / 4u] = v;
	} else if ((i = dsp_mpu(x))) {
		if (m->mpulock != 2) violate("an MPU access word written while the MPU is locked");
		m->mpu[i - 1] = v;
		if (a == 0 && i == 3 && !mark_mpu3) mark_mpu3 = nw;
	} else if (x == 0x2BC3140u) {
		m->mpulock = v == 0x5555u ? 1 : v == 0xAAAAu && m->mpulock == 1 ? 2 : v == 0u ? 3 : (violate("MPU lock order"), 0);
	} else if (x == 0x2BC7000u) {
		m->wdt = v;
	} else if (x == 0x2BC1000u) {
		if ((v & 1u) && !(m->ccm & 1u)) {
			if (m->mpulock != 3 || !m->dsp_mem_nw || m->errata != 0xFFFFu)
				violate("core started before the load / MPU");
			m->starts++;
			if (a == dsp_err_after_start)
				m->st1 |= 0x40u;
		}
		if (!(v & 1u) && (m->ccm & 1u))
			m->stops++;
		m->ccm = v & ~0x200u;	/* model: RESET self-clears */
	}
}
static uint32_t dsp_ctl_rd(int a, uint32_t x)
{
	struct amp *m = &amp[a];
	int i;

	if (dsp_errata(x)) return (m->errata >> ((x & 0x200u ? 8u : 0u) + ((x & 0xFFu) - 0x80u) / 8u)) & 1u;
	if (x >= 0x4C50u && x <= 0x4C5Cu) return m->rxsrc[(x - 0x4C50u) / 4u];
	if ((i = dsp_mpu(x))) return m->mpu[i - 1] & dsp_mpu_rbmask;
	if (x == 0x2BC3140u) return 0u;
	if (x == 0x2BC7000u) return m->wdt;
	if (x == 0x2BC1000u) return m->ccm;
	if (x == 0x2C08u) return 3u;
	if (x >= 0x2B805C0u && x <= 0x2B805D8u) return 0x5C0000u + (x & 0xFFu);
	return 0u;
}
/* a byte of a frame whose address is DSP memory (q = its position, any length: a burst) */
static uint8_t dsp_stream(int a, uint32_t fa, unsigned q, uint8_t mosi, int tx_on)
{
	struct amp *m = &amp[a];
	uint32_t x = fa + 4u * ((q - 6u) / 4u);

	if (q < 6u) {
		if (!tx_on) violate("a burst address / pad byte with the transmitter off");
		if (mosi != 0) violate("a burst pad byte is not 0");
		return 0;
	}
	if (!dsp_mem(x)) violate("a burst runs past its DSP memory range");
	if (!(m->frame[0] & 0x80u) && q - 5u > dsp_wmax) dsp_wmax = q - 5u;
	if ((m->frame[0] & 0x80u) && q - 5u > dsp_rmax) dsp_rmax = q - 5u;
	if (!(m->frame[0] & 0x80u)) {
		if (!tx_on) violate("a burst write byte with the transmitter off");
		m->wacc = (m->wacc << 8) | mosi;
		if ((q - 6u) % 4u == 3u && dsp_mem(x))
			dsp_put(a, x, m->wacc);
		return 0;
	}
	if (!released(a) || !dsp_mem(x))
		return 0;
	return (uint8_t)(dsp_get(a, x) >> (8u * (3u - (q - 6u) % 4u)));
}

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
	if (m->np++ >= 4u) {	/* patch 0381: a DSP memory frame is a burst of any length */
		uint32_t fa = ((uint32_t)m->frame[0] << 24 | (uint32_t)m->frame[1] << 16 | (uint32_t)m->frame[2] << 8 | m->frame[3]) & 0x7fffffffu;

		if (dsp_mem(fa))
			return dsp_stream(a, fa, m->np - 1u, mosi, tx_on);
	}
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
			if (wa == 0x2B80010u || (wa >= 0x13000u && wa <= 0x1305Cu))
				violate("CORE_SOFT_RESET or a mailbox written");
			if (m->nwr >= 12 && dsp_ctl(wa)) {
				dsp_ctl_wr(a, wa, wv);	/* patch 0381 */
			} else if (m->nwr >= 12) {
				otp_wr(a, wa, wv);	/* A7: the OTP unpack after the 12 */
			} else {
				if (SEQ[m->nwr][0] != wa || SEQ[m->nwr][1] != wv) {
					m->seq_bad++;
					violate("a write outside the stock B2 sequence or out of order");
				}
				m->nwr++;
			}
			if (m->st1_late_at && m->nwr >= m->st1_late_at)
				m->st1 |= m->st1_late;
			if (wa == 0x10110u && !m->ignore_mask) m->mask1 = wv;
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
	if (p == 5 && m->frame[0] == 0x80u && m->frame[1] == 0x01u && m->frame[2] == 0x00u && m->frame[3] == 0x10u)
		m->st1_seen = 1;
	if (p == 5) {
		addr = ((uint32_t)m->frame[0] << 24 | (uint32_t)m->frame[1] << 16 | (uint32_t)m->frame[2] << 8 | m->frame[3]) & 0x7fffffffu;
		if (addr != REGS4[0] && addr != REGS4[1] && addr != REGS4[2] && addr != REGS4[3] && addr != 0x10010u && addr != 0x10110u && addr != 0x2BC1000u &&
		    addr != 0x10u && !(addr >= 0x400u && addr <= 0x47Cu && !(addr & 3u)) && !ref_target(addr) && a9_idx(addr) < 0 && !dsp_rd(addr))
			violate("a frame names a register outside the four");
		if (released(a) && m->was_released && first_frame_tick[a] && first_frame_tick[a] - m->t_high < 2000u)
			violate("first frame less than 2 ms after the reset release");
	}
	if (p < 6 || m->absent || !released(a))
		return m->ff ? 0xff : 0;
	addr = ((uint32_t)m->frame[0] << 24 | (uint32_t)m->frame[1] << 16 | (uint32_t)m->frame[2] << 8 | m->frame[3]) & 0x7fffffffu;
	if (addr == 0x1001Cu && p == 6 && !m->booted && ++m->reads_since >= m->boot_polls)
		m->booted = 1;
	if (addr == 0x400u && p == 6)
		m->mem_reads++;
	if (addr == 0x10u || (addr >= 0x400u && addr <= 0x47Cu)) {
		val = addr == 0x10u ? m->otpid : m->mem[(addr - 0x400u) / 4u] ^ (m->mem_flip && m->mem_reads >= 2 && addr == 0x414u ? 1u : 0u);
		return p < 10 ? (uint8_t)(val >> (8 * (9 - p))) : 0;
	}
	if (a9_idx(addr) >= 0) {
		val = m->a9v[a9_idx(addr)];
		return p < 10 ? (uint8_t)(val >> (8 * (9 - p))) : 0;
	}
	if (dsp_rd(addr) && addr != 0x2BC1000u) {
		val = dsp_ctl_rd(a, addr);
		return p < 10 ? (uint8_t)(val >> (8 * (9 - p))) : 0;
	}
	if (addr == 0x10018u && amp_a9_cb.on)
		(void)a9_st3(a);
	if (ref_target(addr)) {
		val = *regp(m->ra, m->rv, &m->nreg, addr);
		return p < 10 ? (uint8_t)(val >> (8 * (9 - p))) : 0;
	}
	val = addr == 0x0u ? (m->devid ? m->devid : 0x35A40u) : addr == 0x4u ? (m->revid ? m->revid : 0xB2u) : addr == 0x1001Cu ? (m->booted ? 0x2u : 0u) : addr == 0x10110u ? m->mask1 :
	      addr == 0x2BC1000u ? m->ccm : addr == 0x10010u ? m->st1 : addr == 0x10018u ? m->st3 : 0u;
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
			amp[a].nf = amp[a].np = 0;	/* chip select released: the next frame starts fresh */
		if (a == 0 && before[0] && !cs_low(0) && amp[0].drop_cs1_after_st1 && amp[0].st1_seen) {
			h.dat[1] &= ~1u;	/* CS1 stuck low from now on */
			amp[0].drop_cs1_after_st1 = 0;
		}
		if (!before[a] && cs_low(a))
			amp[a].nf = amp[a].np = 0;
		if (rel[a] && !released(a) && (amp[a].ccm & 1u))
			amp[a].reset_running = 1;	/* patch 0381: reset while the core ran (the stop was skipped) */
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
	if (flip_at && nw == flip_at)
		h.dat[flip_bank] ^= flip_bit;
	if (wfault_n && nw == wfault_n) {
		wfault_pa = pa;
		return 0;
	}
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
	for (int a = 0; a < 2; a++) {	/* A7: OTP id 1 (map 1) on L, 2 (map 2) on R; pseudo-random OTP words and row registers, the same in every run */
		uint32_t sd = 0x2468ACE1u + (uint32_t)a;

		amp[a].otpid = a == 0 ? 1u : 2u;
		for (int i = 0; i < 32; i++)
			amp[a].mem[i] = sd = sd * 1664525u + 1013904223u;
		for (int mm = 0; mm < 2; mm++)
			for (int i = 0; i < 99; i++)
				if (REFMAPS[mm][i].reg)
					*regp(amp[a].ra, amp[a].rv, &amp[a].nreg, REFMAPS[mm][i].reg) = sd = sd * 1664525u + 1013904223u;
		memcpy(amp[a].ra0, amp[a].ra, sizeof(amp[a].ra));
		memcpy(amp[a].rv0, amp[a].rv, sizeof(amp[a].rv));
		amp[a].nreg0 = amp[a].nreg;
	}
	for (int a = 0; a < 2; a++) {	/* patch 0378 A9: random start words, PWR_CTRL1 with GLOBAL_EN clear */
		for (int i = 0; i < 10; i++)
			amp[a].a9v[i] = rnd();
		amp[a].a9v[10] = rnd() & ~1u;
	}
	memcpy(a9v0[0], amp[0].a9v, sizeof(a9v0[0])); memcpy(a9v0[1], amp[1].a9v, sizeof(a9v0[1]));
	sclk_on = 0; pll_never = 0; a9_opens = a9_closes = 0; a9_open_ret = 1;
	memset(mark_unlock, 0, sizeof(mark_unlock)); memset(mark_lock, 0, sizeof(mark_lock)); memset(mark_otp_end, 0, sizeof(mark_otp_end));
	nw = nreads = nframes = violations = mux_writes = 0; why_v[0] = 0;
	flip_at = 0; flip_bank = 0; flip_bit = 0;
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
		if (h.con[i] != h0.con[i] || (h.dat[i] ^ h0.dat[i]) != (i == flip_bank ? flip_bit : 0u) || h.pud[i] != h0.pud[i] || h.drv[i] != h0.drv[i])
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
	CHECK(has("SWEET AMP: init ok (otp unpack) l,r devid 35a40 rev b2, errors=0"), label);
	CHECK(!has("no otp unpack") && !has("partial"), label);
	CHECK(amp[0].nwr == 12 && amp[1].nwr == 12 && !amp[0].seq_bad && !amp[1].seq_bad, "each amp got exactly the 12 stock writes in order");
	CHECK(otp_as_linux(0) && otp_as_linux(1), "A7: the row registers equal the Linux unpack (L map 1, R map 2)");
	CHECK(amp[0].key == 0 && amp[1].key == 0 && amp[0].lock_seen == 1 && amp[1].lock_seen == 1, "A7: each amp unlocked once and locked again");
	CHECK(has("AMP L otp id=1 map=1") && has("AMP R otp id=2 map=2") && has("readback-bad=0 first=0 lock=ok"), "A7: the otp lines");
	CHECK(restored_exact(), label);
	CHECK(amp[0].frames >= 4 && amp[1].frames >= 4, "both amps answered at least four frames");
	CHECK(nframes <= 2u * (100u + 4u + 12u + 3u + 1u + 64u + 99u + 99u + 4u + 99u), "no more frames than polls + reads + the init + the otp unpack per amp");
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
static void s_rstinhigh(void) { h.dat[2] |= 8u; }	/* review F1: reset pad an INPUT that reads high */
static void s_rstrinhigh(void) { h.dat[3] |= 0x80u; }

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

#ifndef amp_init_run_or_id
#define amp_init_run_or_id amp_init_run
#endif
/* review F2: another pin of a bank changes its input level during the run: still a pass, an exact restore of the amp bits, resets low */
static void flipped(const char *label)
{
	int rc = amp_init_run_or_id();

	CHECK(rc == 0, label);
	CHECK(violations == 0, why_v);
	CHECK(restored_exact(), label);
	CHECK(has("restore=ok"), label);
	printf("ok: other-pin flip: %s\n", label);
}

#ifndef amp_init_run_or_id
#define amp_init_run_or_id amp_init_run
#endif

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
	/* A7 OTP: the four test-key words, the row registers (38, any value), nothing else; armed reads exact */
	CHECK(amp_write_ok(0x40u, 0x55u) && amp_write_ok(0x40u, 0xAAu) && amp_write_ok(0x40u, 0xCCu) && amp_write_ok(0x40u, 0x33u) && !amp_write_ok(0x40u, 0x56u) && !amp_write_ok(0x40u, 0u), "test-key words");
	CHECK(amp_write_ok(0x6E48u, 0x12345678u) && amp_write_ok(0x4000u, 0x800u) && amp_write_ok(0x17044u, 1u) && !amp_write_ok(0x6E68u, 0u) && !amp_write_ok(0u, 0u) && !amp_write_ok(0x400u, 0u) && !amp_write_ok(0x10u, 0u), "OTP row registers only");
	{
		unsigned nt = 0;

		for (uint32_t r = 0; r < 0x20000u; r += 4u)
			nt += amp_otp_reg_ok(r) != 0;
		CHECK(nt == 38u, "38 OTP row registers");
	}
	CHECK(w32_allowed(R_TX, 0u << 6, 0x80u, 0, 0, 2, 0x47Cu, 0) && w32_allowed(R_TX, 2u << 6, 0x04u, 0, 0, 2, 0x47Cu, 0) && w32_allowed(R_TX, 3u << 6, 0x7Cu, 0, 0, 2, 0x47Cu, 0), "armed OTP_MEM31 read frame");
	CHECK(!w32_allowed(R_TX, 3u << 6, 0x80u, 0, 0, 2, 0x47Cu, 0) && !w32_allowed(R_TX, 6u << 6, 0x00u, 0, 0, 2, 0x47Cu, 0), "armed read: another byte / a seventh byte refused");
	CHECK(!w32_allowed(R_TX, 0u << 6, 0x80u, 0, 0, 2, 0x480u, 0) && !w32_allowed(R_TX, 0u << 6, 0x80u, 0, 0, 2, 0x201Cu, 0), "armed read of an unlisted register refused");	/* patch 0384: PWR_CTRL2 0x2018 is read now; PWR_CTRL3 is not */
	CHECK(w32_allowed(R_TX, 0u << 6, 0x80u, 0, 0, 2, 0x2014u, 0), "patch 0378: PWR_CTRL1 may be READ (GLOBAL_EN must read 0), never written");
	CHECK(!w32_allowed(R_PKT, 0, 10u, 0, 0, 2, 0x400u, 0) && !w32_allowed(R_CH, 0, CH_TX | CH_RX, 10u << 6, 0, 2, 0x400u, 0), "an armed read never sends a write frame");
	CHECK(w32_allowed(R_CH, 0, CH_TX | CH_RX, 6u << 6, 0, 2, 0x400u, 0), "an armed read sends its 6-byte frame");
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

/* ---- patch 0381 A9e: synthetic firmware (the stock layout, small) and the DSP cases ---- */
static uint8_t syn_w[512], syn_b[2][256], syn_c[2048];
static size_t syn_wn, syn_bn[2];
static const uint32_t SYN_HDR[24] = { 0x040000u, 0x030000u, 2u, 0x400a4u, 0x1d00u, 0x40u, 0x0eu, 0u, 0u, 2u,	/* id header: fw xm_base 0x40, 2 algs */
				      0xcdu, 0x1d3100u, 0x20u, 0x10u, 0x0eu, 0x40u, 0xf20bu, 1u, 0x30u, 0x08u, 0u, 0u, 0xbedeadu, 0u };
static void le32w(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
/* 4n words -> 3n packed registers, big-endian bytes (as the stock wmfw stores them: the terminator 0xbedead decodes this way) */
static size_t pack_words(uint8_t *out, const uint32_t *w, size_t n)
{
	size_t o = 0;

	for (size_t g = 0; g < n / 4u; g++) {
		unsigned __int128 b = 0;

		for (int i = 3; i >= 0; i--)
			b = (b << 24) | (w[4u * g + (size_t)i] & 0xFFFFFFu);
		for (int j = 0; j < 3; j++) {
			uint32_t r = (uint32_t)(b >> (32 * j));

			out[o++] = (uint8_t)(r >> 24); out[o++] = (uint8_t)(r >> 16); out[o++] = (uint8_t)(r >> 8); out[o++] = (uint8_t)r;
		}
	}
	return o;
}
static size_t region(uint8_t *p, uint32_t type, uint32_t off, const uint8_t *d, uint32_t n)
{
	le32w(p, (off & 0xFFFFFFu) | type << 24);
	le32w(p + 4, n);
	memcpy(p + 8, d, n);
	return 8u + n;
}
static size_t block(uint8_t *p, uint32_t type, uint32_t off, uint32_t id, const uint8_t *d, uint32_t n)
{
	memset(p, 0, 20u + ((n + 3u) & ~3u));
	p[0] = (uint8_t)off; p[1] = (uint8_t)(off >> 8); p[2] = (uint8_t)type; p[3] = (uint8_t)(type >> 8);
	le32w(p + 4, id); le32w(p + 16, n);
	memcpy(p + 20, d, n);
	return (20u + n + 3u) & ~3u;
}
static void syn_build(void)
{
	uint8_t t[96];
	uint32_t code[8] = { 0x111111u, 0x5u, 0x222222u, 0x333333u, 0x444444u, 0x555555u, 0x666666u, 0x777777u }, ymw[12];
	size_t n;

	memset(syn_w, 0, sizeof(syn_w));
	memcpy(syn_w, "WMFW", 4); le32w(syn_w + 4, 40); syn_w[8] = 0; syn_w[9] = 3; syn_w[10] = 4; syn_w[11] = 3;
	n = 40;
	n += region(syn_w + n, 0xFFu, 0, (const uint8_t *)"synthetic", 9);
	n += region(syn_w + n, 0x11u, 0, t, (uint32_t)pack_words(t, SYN_HDR, 24));	/* XM packed 0: id header + list */
	n += region(syn_w + n, 0x11u, 0x40u, t, (uint32_t)pack_words(t, code, 8));	/* XM packed at the fw xm_base (HALO_STATE, HALO_HEARTBEAT) */
	for (int i = 0; i < 12; i++) ymw[i] = 0x100000u + (uint32_t)i;
	n += region(syn_w + n, 0x12u, 0, t, (uint32_t)pack_words(t, ymw, 12));	/* YM packed 0 */
	for (int i = 0; i < 20; i++) t[i] = (uint8_t)(0xA0 + i);
	n += region(syn_w + n, 0x10u, 0, t, 20);					/* PM: one group of 5 registers */
	n += region(syn_w + n, 0xF2u, 0, (const uint8_t *)"algdata!", 8);
	syn_wn = n;
	for (int a = 0; a < 2; a++) {
		uint8_t d[12];

		memset(syn_b[a], 0, sizeof(syn_b[a]));
		memcpy(syn_b[a], "WMDR", 4); le32w(syn_b[a] + 4, 16); syn_b[a][8] = 0; syn_b[a][9] = 0x1d; syn_b[a][10] = 0; syn_b[a][11] = 1;
		n = 16;
		n += block(syn_b[a] + n, 0xFE00u, 0, 0, (const uint8_t *)"name!", 5);
		for (int i = 0; i < 12; i++) d[i] = (uint8_t)(0x30 + 16 * a + i);
		n += block(syn_b[a] + n, 0x12u, 0, 0xcdu, d, 12);			/* YM packed of alg 0xcd: reg (0x2C00000 + 0x0e * 3) & ~3 */
		memset(d, 0, 8); d[3] = (uint8_t)(0x41 + a); d[7] = 0x42;
		n += block(syn_b[a] + n, 0x6u, 8, 0xcdu, d, 8);			/* YM unpacked of alg 0xcd: 0x3400000 + 4 * 0x0e + 8 */
		syn_bn[a] = n;
	}
}
/* patch 0382: syn_w + three long regions (XM packed 1212 B, YM unpacked 1004 B, PM 1000 B: 3 bursts each) before the algorithm data */
static uint8_t syn_w2[4096];
static size_t syn_wn2;
static const uint32_t LR_REG[3] = { 0x2000300u, 0x3400400u, 0x3800028u }, LR_LEN[3] = { 1212u, 1004u, 1000u };
static uint8_t lr_d[3][1212];
static void syn_build2(void)
{
	static const uint32_t ty[3] = { 0x11u, 0x6u, 0x10u }, of[3] = { 0x100u, 0x100u, 8u };
	size_t n = syn_wn - 16u;	/* drop the trailing 0xF2 region (8 bytes + 8 header) */

	memcpy(syn_w2, syn_w, n);
	for (int r = 0; r < 3; r++) {
		for (uint32_t i = 0; i < LR_LEN[r]; i++)
			lr_d[r][i] = (r == 1 && !(i & 3u)) ? 0u : (uint8_t)(1u + (i * 7u + (uint32_t)r * 13u) % 251u);	/* never 0 (YM unpacked: top byte 0) */
		n += region(syn_w2 + n, ty[r], of[r], lr_d[r], LR_LEN[r]);
	}
	n += region(syn_w2 + n, 0xF2u, 0, (const uint8_t *)"algdata!", 8);
	syn_wn2 = n;
}
/* every register of the long regions holds what ONE burst of the region would have left there (the 4 stream bytes, big endian) */
static int lr_image_ok(int a)
{
	for (int r = 0; r < 3; r++)
		for (uint32_t i = 0; i < LR_LEN[r]; i += 4u) {
			uint32_t want = (uint32_t)lr_d[r][i] << 24 | (uint32_t)lr_d[r][i + 1] << 16 | (uint32_t)lr_d[r][i + 2] << 8 | lr_d[r][i + 3];
			uint32_t got = dsp_get(a, LR_REG[r] + i);

			if (r == 1 ? (got & 0xFFFFFFu) != (want & 0xFFFFFFu) : got != want)
				return 0;
		}
	return 1;
}
static void dsp_cfg(int with_fw)
{
	amp_dsp_fw_in = (struct amp_dsp_fw){ { 0 }, { 0 } };
	if (with_fw) {
		amp_dsp_fw_in.d[0] = syn_w; amp_dsp_fw_in.n[0] = (uint32_t)syn_wn;
		amp_dsp_fw_in.d[1] = syn_b[0]; amp_dsp_fw_in.n[1] = (uint32_t)syn_bn[0];
		amp_dsp_fw_in.d[2] = syn_b[1]; amp_dsp_fw_in.n[2] = (uint32_t)syn_bn[1];
	}
	hb_dead = hb_needs_sclk = 0; dsp_err_after_start = -1; dsp_stuck_amp = -1; dsp_stuck_addr = 0; dsp_wmax = dsp_rmax = 0;
	dsp_mpu_rbmask = 0xFFFFFFFFu; mark_mpu3 = 0;
}
static void dsp_run(int with_fw)
{
	reset_hw(); dsp_cfg(with_fw); tick_need = 20; tick_bad = 0;
	amp_a9_cb = (struct amp_a9_cb){ 1, cb_open5, cb_close, NULL, cb_tick, 500u, 1, NULL };
}
static int dsp_go(void)
{
	int rc = amp_init_run();

	amp_a9_cb = (struct amp_a9_cb){ 0 };
	return rc;
}
static void dsp_tests(void)
{
	static const uint8_t abc_sha[32] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
					     0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
	uint8_t sha[32];
	int rc;

	syn_build();
	amp_sha256((const uint8_t *)"abc", 3, sha);
	CHECK(!memcmp(sha, abc_sha, 32), "sha256(abc)");
	{	/* 1000 bytes: several blocks, the length in the last one */
		static uint8_t z[1000];
		static const uint8_t want[4] = { 0x54, 0x1b, 0x3e, 0x9d };	/* sha256 of 1000 zero bytes starts 541b3e9d */
		amp_sha256(z, sizeof(z), sha);
		CHECK(!memcmp(sha, want, 4), "sha256(1000 x 0)");
	}
	/* the allowlist */
	CHECK(!amp_write_ok(0x2014u, 1u) && !amp_dsp_ctl_ok(0x2014u, 0u), "PWR_CTRL1 never");
	CHECK(!amp_write_ok(0x2BC1000u, 0x201u) && !amp_write_ok(0x2BC7000u, 0u), "CCM start / WDT not writable unarmed");
	CHECK(!amp_write_ok(0x13020u, 2u) && !amp_write_ok(0x2B80010u, 1u) && !amp_write_ok(0x2C08u, 3u), "no mailbox, SOFT_RESET, DSP_CLK write");
	CHECK(amp_write_ok(0x2B80080u, 1u) && !amp_write_ok(0x2B80080u, 0u) && !amp_write_ok(0x2B80084u, 1u) && amp_write_ok(0x4C58u, 0x3Au) &&
	      !amp_write_ok(0x4C58u, 0x28u) && amp_write_ok(0x2BC305Cu, 0xFFFFFFFFu) && !amp_write_ok(0x2BC3010u, 0xFFFFFFFFu), "control pairs exact");
	CHECK(amp_dsp_mem_ok(0x3800000u, 0x4FECu) && !amp_dsp_mem_ok(0x3800000u, 0x4FF0u) && !amp_dsp_mem_ok(0x2002FF0u, 8u) && !amp_dsp_mem_ok(0x2001002u, 4u) &&
	      !amp_dsp_mem_ok(0x2000000u, 6u) && amp_dsp_mem_ok(0x2C017F0u, 4u) && !amp_dsp_mem_ok(0x2BC1000u, 4u), "memory ranges");
	amp_dsp_row.on = 1; amp_dsp_row.reg = 0x2BC1000u; amp_dsp_row.mask = 0x201u; amp_dsp_row.field = 0x201u; amp_dsp_row.old = 0x10u;
	CHECK(amp_write_ok(0x2BC1000u, 0x211u) && !amp_write_ok(0x2BC1000u, 0x201u) && !amp_write_ok(0x2BC7000u, 0x211u) && !amp_write_ok(0x2014u, 0x211u), "armed row");
	amp_dsp_row.on = 0;
	CHECK(amp_dsp_bin_reg(0x12u, 0x0eu, 0x40u, 0u, 12u) == 0x2C00028u && amp_dsp_bin_reg(0x6u, 0x0eu, 0x40u, 8u, 8u) == 0x3400040u &&
	      amp_dsp_bin_reg(0x6u, 0x0eu, 0x2u, 8u, 8u) == 0u && amp_dsp_bin_reg(0x12u, 0x0eu, 0x40u, 0x10000u, 4u) == 0u, "bin addressing (Google load_coeff) and bounds");
	printf("ok: A9e sha256, allowlist, ranges, bin addressing\n");

	/* good: both loaded, started after the TDM config, heartbeat in window 1, stopped, restored */
	dsp_run(1); rc = dsp_go();
	CHECK(rc == 0 && violations == 0, why_v);
	CHECK(has("SWEET AMP DSP: fw loaded l,r (id 400a4 rev 1d00), halo running l,r"), "A9e pass line");
	CHECK(amp_dsp_res.a[0].win == 1 && amp_dsp_res.a[1].win == 1 && amp[0].starts == 1 && amp[1].starts == 1 && amp[0].stops == 1 && amp[1].stops == 1,
	      "started once, ran in window 1, stopped once");
	CHECK(!amp[0].reset_running && !amp[1].reset_running && amp[0].mpulock == 3 && amp[0].errata == 0xFFFFu && amp[1].rxsrc[3] == 0x3Bu, "stop before the reset, MPU, errata, RX sources");
	CHECK(amp[0].ymp_first == 0x2C00000u && amp[1].ymu_first == 0x3400040u && amp[0].ym[0x10] == 0x41u && amp[1].ym[0x10] == 0x42u && amp[0].ym[0x11] == 0x42u,
	      "bin blocks at the stock addresses, R gets R-.bin");
	CHECK(amp[0].xm[3] == 0x400a4u && amp[0].xm[22] == 0xbedeadu && amp[1].pm[4] == 0xB0B1B2B3u, "wmfw regions in memory (packed view decodes)");
	CHECK(amp[0].a9v[10] == a9v0[0][10] && amp[1].a9v[10] == a9v0[1][10] && restored_exact() && a9_closes == 1 && !tick_bad, "PWR_CTRL1 untouched, restore exact, AOC closed");
	printf("ok: A9e good: %u + %u memory writes, halo running l,r (window 1), stopped, restore exact\n", amp[0].dsp_mem_nw, amp[1].dsp_mem_nw);
	{	/* patch 0382: regions longer than one burst: 3 bursts each, the memory image of the single burst, every register written once */
		unsigned nw0 = amp[0].dsp_mem_nw, nw1 = amp[1].dsp_mem_nw;

		syn_build2();
		dsp_run(1); amp_dsp_fw_in.d[0] = syn_w2; amp_dsp_fw_in.n[0] = (uint32_t)syn_wn2; rc = dsp_go();
		CHECK(rc == 0 && violations == 0 && has("SWEET AMP DSP: fw loaded l,r (id 400a4 rev 1d00), halo running l,r"), "A9f long regions: pass line");
		CHECK(dsp_wmax == AMP_DSP_BURST && dsp_rmax == AMP_DSP_BURST && AMP_DSP_BURST <= 512u, "A9f: no write / read burst longer than 480 bytes, 480 used");
		CHECK(lr_image_ok(0) && lr_image_ok(1), "A9f: memory = the single-burst image (XM packed, YM unpacked, PM across the 480 / 960 boundaries)");
		CHECK(amp[0].dsp_mem_nw == nw0 + (1212u + 1004u + 1000u) / 4u && amp[1].dsp_mem_nw == nw1 + (1212u + 1004u + 1000u) / 4u,
		      "A9f: every register written exactly once (no overlap, no gap at a burst boundary)");
		CHECK(amp_dsp_res.a[0].regions == 7u && amp_dsp_res.a[0].fk == 0, "A9f: 7 memory regions, no failure recorded");
		printf("ok: A9f bursts <= %u B: 3 long regions per amp land as one burst would, %u + %u memory writes\n", AMP_DSP_BURST, amp[0].dsp_mem_nw,
		       amp[1].dsp_mem_nw);

		/* a PM register in the second burst does not stick on L: the line names the read-back, its offset and both words; R not written */
		dsp_run(1); amp_dsp_fw_in.d[0] = syn_w2; amp_dsp_fw_in.n[0] = (uint32_t)syn_wn2; dsp_stuck_amp = 0; dsp_stuck_addr = 0x3800028u + 520u; rc = dsp_go();
		{
			char want[96];
			uint32_t w = (uint32_t)lr_d[2][520] << 24 | (uint32_t)lr_d[2][521] << 16 | (uint32_t)lr_d[2][522] << 8 | lr_d[2][523];

			snprintf(want, sizeof(want), "SWEET AMP DSP: L wmfw-region#7 read-back mismatch +520/1000 exp %08x got 00000000", w);
			CHECK(violations == 0 && amp_dsp_res.a[0].stage == DS_WMFW && amp_dsp_res.a[0].fk == 3 && has(want) &&
			      has("SWEET AMP DSP: stopped l=wmfw-region#7/rb r=not-run stop -/- restore ok") && amp[1].dsp_mem_nw == 0u && !amp[0].starts &&
			      restored_exact(), "A9f read-back mismatch line");
		}
		printf("ok: A9f read-back mismatch in burst 2: offset 520, expected / got word printed, no start, R untouched\n");
	}

	/* runs only once the stream clocks: window 2 */
	dsp_run(1); hb_needs_sclk = 1; rc = dsp_go();
	CHECK(rc == 0 && violations == 0 && amp_dsp_res.a[0].win == 2 && amp_dsp_res.a[1].win == 2 && has("halo running l,r") && !amp[0].reset_running, "A9e window 2");
	printf("ok: A9e heartbeat only with the stream: running in window 2, stopped\n");

	/* never runs: both windows bounded, stop on both, partial line */
	dsp_run(1); hb_dead = 1; rc = dsp_go();
	CHECK(violations == 0 && amp_dsp_res.a[0].polls == 2u * AMP_DSP_HB_POLLS && amp[0].stops == 1 && amp[1].stops == 1 && !amp[0].reset_running &&
	      !amp[1].reset_running && has("SWEET AMP DSP: stopped l=no-heartbeat r=no-heartbeat stop ok/ok restore ok") && restored_exact(), "A9e no heartbeat");
	printf("ok: A9e no heartbeat: 2 x %u polls, core stopped l,r before the reset\n", AMP_DSP_HB_POLLS);

	/* an error bit after the start on L: L stopped, line says so */
	dsp_run(1); dsp_err_after_start = 0; rc = dsp_go();
	CHECK(violations == 0 && amp_dsp_res.a[0].stage == DS_ERR && amp[0].stops == 1 && amp[1].stops == 1 && !amp[0].reset_running &&
	      has("l=error-after-start"), "A9e error after start");
	printf("ok: A9e error bit after the start: stopped, loud\n");
	/* patch 0383: the start / run stage detail lines */
	CHECK(has("SWEET AMP DSP: L step=post-st1-err/w1 reg=00010010 w=00000000 r=00000040 e=0 pwr1=") &&
	      has("SWEET AMP DSP: L mpu rb 20/20 not-ffffffff=0") && has("SWEET AMP DSP: R mpu rb 20/20 not-ffffffff=0"), "A9g error after start: detail line");
	dsp_run(1); hb_dead = 1; rc = dsp_go();
	CHECK(violations == 0 && has("SWEET AMP DSP: L step=hb-still/w2 reg=") && has("SWEET AMP DSP: R step=hb-still/w2 reg="), "A9g no heartbeat: detail line");
	printf("ok: A9g detail lines: post-st1-err/w1 (st1, pwr1), hb-still/w2\n");

	/* patch 0383: MPU access words that do not read back all ones (stock never reads them): logged, the core starts, pass */
	dsp_run(1); dsp_mpu_rbmask = 0x0000FFFFu; rc = dsp_go();
	CHECK(rc == 0 && violations == 0 && amp[0].starts == 1 && amp[1].starts == 1 && amp[0].mpulock == 3 && amp[1].mpulock == 3 &&
	      has("SWEET AMP DSP: L mpu rb 20/20 not-ffffffff=20 first 02bc3000=0000ffff") &&
	      has("SWEET AMP DSP: R mpu rb 20/20 not-ffffffff=20 first 02bc3000=0000ffff") &&
	      has("SWEET AMP DSP: fw loaded l,r (id 400a4 rev 1d00), halo running l,r") && restored_exact(), "A9g MPU read-back is a logged value");
	printf("ok: A9g MPU words read 0000ffff: logged (20/20 not-ffffffff), started, pass line\n");

	/* patch 0383: a failing SPI write on L's 3rd MPU access word: step named, the MPU relocked (LOCK_CONFIG := 0), no core started, R untouched */
	{
		unsigned m3, nrl = 0, nacc = 0, ncs = 0, rr;

		dsp_run(1); rc = dsp_go(); m3 = mark_mpu3;
		CHECK(m3 > 400u, "A9g: the 3rd L MPU access write seen");
		for (unsigned k = m3 - 400u; k <= m3 + 400u; k++) {	/* every MMIO write position around the L MPU access frames */
			dsp_run(1); wfault_n = k; rc = dsp_go();
			if (amp_dsp_res.a[0].stage != DS_MPU || !has("SWEET AMP DSP: L step=access reg=02bc30"))
				continue;	/* the fault hit another frame */
			nacc++;
			/* a dropped chip-select pad write leaves L's CS low; a later L read (amp_read checks only the OTHER CS, 0373;
			 * guess: the a9 reads after the DSP stop) extends the open frame. Measured pre-existing: the same 25 positions with the relock removed. */
			if (bank_of(wfault_pa, &rr) >= 0 && violations && !strcmp(why_v, "a write frame longer than 10 bytes"))
				ncs++;
			else
				CHECK(violations == 0, why_v);
			CHECK(!amp[0].starts && !amp[1].starts && amp[1].mpulock == 0, "A9g: failed MPU write: no start, R MPU untouched");
			CHECK(has(" w=ffffffff r=00000000 e=") && has("SWEET AMP DSP: stopped l=mpu r=loaded-not-started"), "A9g: failed MPU write: step named");
			CHECK(!has("restore ok") || restored_exact(), "A9g: failed MPU write: restore ok only when exact");
			nrl += amp[0].mpulock == 3;
		}
		CHECK(nacc > 0u && nrl > 0u, "A9g: a failed MPU access write is followed by the relock");
		printf("ok: A9g failed MPU access write at %u fault positions: step=access named, no start; relocked in %u (others: controller / CS stuck; "
		       "%u CS-pad faults extend a later L read frame, pre-existing)\n", nacc, nrl, ncs);
	}

	/* a PM word on R does not stick: R stops at its wmfw region, nothing is started */
	dsp_run(1); dsp_stuck_amp = 1; dsp_stuck_addr = 0x3800008u; rc = dsp_go();
	CHECK(violations == 0 && amp_dsp_res.a[1].stage == DS_WMFW && amp_dsp_res.a[0].stage == DS_LOADED && !amp[0].starts && !amp[1].starts &&
	      has("SWEET AMP DSP: stopped l=loaded-not-started r=wmfw-region#") && restored_exact(), "A9e read-back mismatch");
	printf("ok: A9e read-back mismatch: stage named, no core started\n");

	/* a wrong terminator (L): alg-header stop, no .bin write, R untouched */
	dsp_run(1); dsp_stuck_amp = 0; dsp_stuck_addr = 0x2000000u + 4u * 16u; rc = dsp_go();	/* the register holding word 22 (terminator) */
	CHECK(violations == 0 && amp_dsp_res.a[0].stage != DS_LOADED && amp[0].ymu_first == 0u && amp[1].dsp_mem_nw == 0u && !amp[0].starts, "A9e bad header / region");
	printf("ok: A9e L header region bad: L stopped (%s), no bin write, R not written\n", amp_dsp_stage_name[amp_dsp_res.a[0].stage]);

	/* no files: no DSP write at all, the sweetaoc=5 part still runs */
	dsp_run(0); rc = dsp_go();
	CHECK(rc == 0 && violations == 0 && !amp[0].dsp_ctl_nw && !amp[1].dsp_ctl_nw && !amp[0].dsp_mem_nw && a9_opens == 1 && a9_closes == 1 &&
	      has("SWEET AMP DSP: stopped l=no-fw r=not-run stop -/- restore ok") && restored_exact(), "A9e no files");
	printf("ok: A9e no AMPFW: no DSP write, stream part unchanged\n");

	/* the container: a synthetic one is refused (not the pinned files) */
	{
		size_t o = 16 + 48;
		memset(syn_c, 0, sizeof(syn_c));
		memcpy(syn_c, "AMPFWV01", 8); le32w(syn_c + 8, 1); le32w(syn_c + 12, 3);
		memcpy(syn_c + 16, "WMFW", 4); le32w(syn_c + 20, (uint32_t)syn_wn); le32w(syn_c + 24, (uint32_t)o);
		CHECK(amp_dsp_container(syn_c, sizeof(syn_c), &amp_dsp_fw_in) < 0 && !amp_dsp_fw_in.d[0], "unpinned container refused");
		CHECK(amp_dsp_container(syn_c, 10, &amp_dsp_fw_in) == -1, "short container refused");
	}
	/* the stock files, when present: the pinned container check and a full run on them */
	{
		const char *dir = getenv("SWEET_CS35L41_FW");
		char path[600], def[512];
		static const char *const nm[3] = { "cs35l41-dsp1-spk-prot.wmfw", "cs35l41-dsp1-spk-prot.bin", "R-cs35l41-dsp1-spk-prot.bin" };
		static uint8_t c[64 + 34000 + 8000];
		size_t o = 64, k;
		int ok = 1;

		if (!dir) { snprintf(def, sizeof(def), "%s/pixelios-artifacts/aoc/cs35l41", getenv("HOME") ? getenv("HOME") : "."); dir = def; }
		memset(c, 0, sizeof(c)); memcpy(c, "AMPFWV01", 8); le32w(c + 8, 1); le32w(c + 12, 3);
		for (int i = 0; i < 3 && ok; i++) {
			FILE *f;
			snprintf(path, sizeof(path), "%s/%s", dir, nm[i]);
			if (!(f = fopen(path, "rb"))) { ok = 0; break; }
			k = fread(c + o, 1, sizeof(c) - o, f);
			fclose(f);
			memcpy(c + 16 + 16 * i, i == 0 ? "WMFW" : i == 1 ? "BINL" : "BINR", 4); le32w(c + 20 + 16 * i, (uint32_t)k); le32w(c + 24 + 16 * i, (uint32_t)o);
			o = (o + k + 7u) & ~(size_t)7u;
		}
		if (!ok) {
			printf("skip: stock CS35L41 files not present in %s\n", dir);
			return;
		}
		CHECK(amp_dsp_container(c, o, &amp_dsp_fw_in) == 0, "the stock files match the pinned lengths + sha256");
		c[100] ^= 1;
		CHECK(amp_dsp_container(c, o, &amp_dsp_fw_in) == -6, "a flipped byte fails the sha256");
		c[100] ^= 1;
		reset_hw(); dsp_cfg(0); tick_need = 20; tick_bad = 0;
		CHECK(amp_dsp_container(c, o, &amp_dsp_fw_in) == 0, "container");
		amp_a9_cb = (struct amp_a9_cb){ 1, cb_open5, cb_close, NULL, cb_tick, 500u, 1, NULL };
		rc = dsp_go();
		CHECK(rc == 0 && violations == 0 && has("SWEET AMP DSP: fw loaded l,r (id 400a4 rev 1d00), halo running l,r"), "stock files: pass line");
		CHECK(amp_dsp_res.a[0].hb_reg == 0x2800784u && amp_dsp_res.a[0].cd_xm == 0x94u && amp_dsp_res.a[0].cd_ym == 0x0eu && amp_dsp_res.a[0].regions == 6u &&
		      amp_dsp_res.a[0].blocks == 2u && amp[0].ymp_first == 0x2C00000u, "stock: heartbeat 0x2800784, CSPL xm 0x94 ym 0xe, 6 regions, 2 blocks");
		CHECK(amp[0].ymu_first == 0x3401130u && amp[1].ymu_first == 0x3401110u, "stock: YM unpacked blocks L 0x3401130, R 0x3401110");
		CHECK(!amp[0].reset_running && !amp[1].reset_running && restored_exact(), "stock: stopped, restored");
		printf("ok: A9e stock files: pinned, %u bytes per amp loaded and read back, halo running l,r, stopped (%u frames)\n", amp_dsp_res.a[0].bytes, nframes);
	}
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
	refused("L reset input reading high (review F1)", s_rstinhigh, "reset");
	refused("R reset input reading high (review F1)", s_rstrinhigh, "reset");

	/* stops after writes */
	reset_hw(); ch_snap_inject = CH_RX; reset_hw(); ch_snap_inject = 0; stopped("CH_CFG has a channel on after the mode write", 1);
	CHECK(has("controller not idle"), "the reason");
	reset_hw(); rfault_pa = R_CH; stopped("controller unreadable after the mode write", 1);
	reset_hw(); amp[0].absent = 1; amp[1].absent = 1; stopped("no amp answers (MISO 0)", 1);
	CHECK(has("SWEET AMP RESULT: l=none r=none restore=ok"), "no answer: no values in the result line");
	CHECK(has("twice"), "zero twice reported");
	reset_hw(); amp[0].ff = 1; amp[0].absent = 1; stopped("L MISO stuck high", 1);
	CHECK(has("l=none") && has("devid=ffffffff"), "ones: no pass value, the raw words in the AMP L line");
	reset_hw(); amp[1].boot_polls = 1000; stopped("R never OTP_BOOT_DONE", 1);
	CHECK(has("AMP R otp=0"), "otp timeout reported");
	CHECK(has("r=none") && !has("r=0x35a40/0xb2"), "no OTP_BOOT_DONE: no pass value in the result line (review F3)");
	reset_hw(); amp[0].st3 = 0x80000000u; stopped("L OTP_BOOT_ERR", 1);
	CHECK(has("l=none") && !has("l=0x35a40/0xb2") && has("OTP_BOOT_ERR"), "OTP_BOOT_ERR: no pass value in the result line (review F3)");
	reset_hw(); flip_at = 200; flip_bank = 2; flip_bit = 4u; flipped("gpp16-2 (BT_REG_ON) input flips mid-run (review F2 H7)");
	CHECK(!released(0) && !released(1) && has("restore=ok"), "H7: resets low, restore ok");
	reset_hw(); h.con[2] |= 0x1000u; h0 = h; flip_at = 200; flip_bank = 2; flip_bit = 4u; flipped("H7b: same, reset started as output low");
	CHECK(!released(0) && !released(1) && has("restore=ok"), "H7b: resets low, restore ok");
	CHECK(!(h.dat[2] & 8u), "H7b: the reset bit is 0");
	reset_hw(); flip_at = 1; flip_bank = 0; flip_bit = 2u; (void)0;
	{ unsigned tot; reset_hw(); (void)amp_init_run_or_id(); tot = nw; reset_hw(); flip_at = tot - 8u; flip_bank = 0; flip_bit = 2u; flipped("H8: MOSI reads high during the restore"); }
	CHECK(has("restore=ok"), "H8: restore ok");
	reset_hw(); amp[1].st1 = 0x40u; stopped("R reports BST_OVP_ERR before the init (pre-write read)", 1);
	CHECK(has("!!! amp error bit") && has("SWEET AMP: init FAILED"), "error bit reported, no pass");
	reset_hw(); amp[0].absent = 1; stopped("L absent: no write frame to it", 1);
	CHECK(amp[0].nwr == 0, "no write to an unidentified amp");
	/* review F1: OTP_BOOT_ERR set: no write to that amp, no pass line */
	reset_hw(); amp[0].st3 = 0x80000000u; stopped("L OTP_BOOT_ERR (A7 gate, review F1)", 1);
	CHECK(amp[0].nwr == 0 && !has("init ok"), "OTP_BOOT_ERR: no write, no pass line");
	/* review F2: the surviving mutants */
	reset_hw(); amp[0].boot_polls = 1000; stopped("L never OTP_BOOT_DONE (gate needs l.otp)", 1);
	CHECK(amp[0].nwr == 0 && !has("init ok"), "no OTP_BOOT_DONE: no write to L");
	reset_hw(); amp[1].revid = 0xB0u; stopped("R is revision B0 (gate needs r.revid)", 1);
	CHECK(amp[1].nwr == 0 && !has("init ok"), "not B2: no write to R");
	reset_hw(); amp[0].st1 = 0x80u; stopped("L BST_DCM_UVP_ERR (error bits include 0x80)", 1);
	CHECK(!has("init ok") && has("init FAILED") && amp[0].nwr == 0, "BST_DCM_UVP_ERR before the init: no write, no pass");
	CHECK(amp[1].nwr == 0, "review F4: no R write after a failed L init");
	reset_hw(); amp[0].st1 = 0x10000u; stopped("L bit 16 (review F6)", 1);
	CHECK(!has("init ok") && amp[0].nwr == 0, "bit 16 before the init: no write");
	reset_hw(); amp[0].ignore_mask = 1; stopped("L IRQ1_MASK1 write does not stick (read-backs compared)", 1);
	CHECK(!has("init ok") && has("init FAILED"), "read-back mismatch: no pass");
	reset_hw(); amp[0].drop_cs1_after_st1 = 1; stopped("CS1 stuck low before the L writes (amp_write checks the other CS)", 1);
	CHECK(amp[0].nwr == 0 && !has("init ok"), "the other chip select low: no write frame");
	{
		static const uint32_t bits[7] = { 0x80000000u, 0x20000u, 0x10000u, 0x8000u, 0x100u, 0x80u, 0x40u };	/* cs35l41.h:684-689 + bit 16 */
		static const unsigned at[2] = { 1u, 12u };

		for (int b = 0; b < 7; b++)
			for (int a = 0; a < 2; a++)
				for (int k = 0; k < 2; k++) {
					reset_hw();
					amp[a].st1_late = bits[b];
					amp[a].st1_late_at = at[k];
					rc = amp_init_run();
					CHECK(rc == 1 && violations == 0, "a post-write error bit fails the run");
					CHECK(has("!!! amp error bit in IRQ1_STATUS1 (H8"), "the post-write check reports it");
					CHECK(!has("init ok"), "no pass line");
					CHECK(a == 1 || amp[1].nwr == 0, "no R write after an L error bit");
					CHECK(restored_exact() && has("restore=ok"), "restored");
				}
		printf("ok: each of the 7 error bits after write 1 or 12, on each amp: fail, reported, no R write after L, restored\n");
	}
	reset_hw(); amp[1].devid = 0x35B40u; stopped("R DEVID is CS35L41R 0x35B40 (gate needs devid)", 1);
	CHECK(amp[1].nwr == 0 && !has("init ok"), "wrong DEVID: no write to R");
	reset_hw(); amp[0].st3 = 0x20000000u; amp[1].st3 = 0x20000000u; rc = amp_init_run();
	CHECK(rc == 0 && has("init ok (otp unpack)") && amp[0].nwr == 12 && amp[1].nwr == 12, "st3 = 0x20000000 (the observed hardware value, bit 29) passes");
	printf("ok: st3 = 0x20000000 on both amps (observed o5-a1-ampid) passes\n");
	reset_hw(); no_clock = 1; stopped("the bus never clocks (RX timeout)", 1);
	CHECK(has("timeout"), "timeout reported");
	reset_hw(); (void)amp_init_run(); unsigned nfull = nw;
	unsigned mu[2] = { mark_unlock[0], mark_unlock[1] }, ml[2] = { mark_lock[0], mark_lock[1] }, me[2] = { mark_otp_end[0], mark_otp_end[1] };
	unsigned nk = 0, nrel = 0, nlf = 0;
	CHECK(mu[0] && ml[0] > mu[0] && me[0] > ml[0] && mu[1] > me[0] && ml[1] > mu[1] && me[1] > ml[1], "A7: the unlock / lock marks of a full run");
	for (unsigned k = 1; k <= nfull; k++) {	/* every write position of a full run, the restore included; inside the OTP row writes every 5th (run time) */
		int inrows = (k > mu[0] + 40u && k + 40u < ml[0]) || (k > mu[1] + 40u && k + 40u < ml[1]);

		if (inrows && k % 5u)
			continue;
		nk++;
		reset_hw();
		wfault_n = k;
		rc = amp_init_run();
		CHECK(rc == 1, "a faulting write is a failure");
		CHECK(violations == 0, why_v);
		CHECK(!has("restore=ok") || restored_exact(), "restore=ok only when exact");
		/* A7: once unlocked, the run relocks or says lock=FAILED */
		CHECK((amp[0].key == 0 && amp[1].key == 0) || has("lock=FAILED"), "A7: a fault after the unlock: relocked or lock=FAILED");
		if (k > mu[0] && k < ml[0]) {	/* a fault between the L unlock and its lock: the L lock goes out (or, chip select stuck low, lock=FAILED); R untouched */
			int relocked = amp[0].key == 0 && amp[0].lock_seen == 1 && has("lock=ok FAILED");

			CHECK(amp[1].nwr == 0 && (relocked || (amp[0].key == 2 && has("lock=FAILED") && has("!!! otp test-key LOCK"))), "A7: L relocked or loud after a fault in its rows");
			nrel += relocked;
			nlf += !relocked;
		}
	}
	CHECK(nrel > 400u, "A7: faults in the L rows relock (a fault that leaves the controller or a chip select stuck cannot: lock=FAILED)");
	printf("ok: a faulting write at %u of %u write positions of a full run stops, restores, never claims a false ok; L rows: %u relocked, %u lock=FAILED (controller / CS stuck by the fault)\n", nk, nfull, nrel, nlf);
	/* A7 OTP stop paths */
	reset_hw(); amp[0].otpid = 5u; stopped("L OTP id 5 has no map", 1);
	CHECK(amp[0].otp_nwr == 0 && amp[1].nwr == 0 && has("no otp map") && has("lock=untouched FAILED"), "unknown id: no unlock, no R write");
	reset_hw(); amp[0].mem_flip = 1; stopped("L OTP word 5 reads differently the second time", 1);
	CHECK(amp[0].otp_nwr == 0 && amp[1].nwr == 0 && has("otp words differ"), "unstable OTP read: no unlock, no R write");
	reset_hw(); amp[1].stuck_reg = 0x6E48u; stopped("R VOFF_GAIN_0..2 write does not stick", 1);
	CHECK(amp[1].key == 0 && amp[1].lock_seen == 1 && has("first=6e48 lock=ok FAILED") && !has("init ok"), "read-back mismatch: relocked, reported, no pass");
	reset_hw(); amp[0].otpid = 8u; amp[1].otpid = 6u; rc = amp_init_run();
	CHECK(rc == 0 && violations == 0 && otp_as_linux(0) && otp_as_linux(1) && has("otp id=8 map=1") && has("otp id=6 map=2"), "ids 8 / 6 select maps 1 / 2 as Linux");
	reset_hw(); amp[0].otpid = 3u; amp[1].otpid = 1u; rc = amp_init_run();
	CHECK(rc == 0 && violations == 0 && otp_as_linux(0) && otp_as_linux(1), "ids 3 / 1");
	{	/* a fault in the first lock word of L: reported lock=FAILED, no pass, no R write */
		reset_hw(); wfault_n = ml[0] - 3u; rc = amp_init_run();
		CHECK(rc == 1 && has("lock=FAILED") && amp[1].nwr == 0, "A7: a lost lock word is a failure");
	}
	printf("ok: A7 otp stop paths (unknown id, unstable OTP, read-back mismatch, lost lock word) and ids 8/6/3/1\n");
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
	/* ---- patch 0378 A9b: TDM config, AOC open / close callbacks, PLL lock, never PWR_CTRL1 ---- */
	{
		int a9ok;

		/* the allowlist: PWR_CTRL1 never, even with a row armed; an armed row only with its field and the other bits as read */
		CHECK(!amp_write_ok(0x2014u, 0u) && !amp_write_ok(0x2014u, 1u), "PWR_CTRL1 refused (unarmed)");
		amp_a9.on = 1; amp_a9.idx = 0; amp_a9.old = 0x12345600u;
		CHECK(amp_write_ok(0x3810u, 0x12345632u), "armed K1 row allows its exact merge");
		CHECK(!amp_write_ok(0x3810u, 0x12345633u) && !amp_write_ok(0x3810u, 0x12345732u) && !amp_write_ok(0x3814u, 0x12345632u), "armed row: other field / bits / register refused");
		CHECK(!amp_write_ok(0x2014u, 0x1u) && !amp_write_ok(0x2014u, 0x12345632u), "PWR_CTRL1 refused with a row armed");
		for (uint32_t i = 0; i < AMP_A9_N; i++) {
			amp_a9.idx = i;
			CHECK(amp_a9_tab[i].reg != 0x2014u && !amp_write_ok(0x2014u, amp_a9.old | 1u), "no row names PWR_CTRL1");
		}
		amp_a9.on = 0; amp_a9.idx = 0; amp_a9.old = 0;
		CHECK(!amp_write_ok(0x3810u, 0x32u) && !amp_write_ok(0x2C04u, 0x510u), "unarmed A9 register refused");
		printf("ok: A9 allowlist (rows armed one at a time, PWR_CTRL1 0x2014 never writable)\n");

		reset_hw(); amp_a9_cb = (struct amp_a9_cb){ 1, cb_open, cb_close, NULL, NULL, 0u, 0, NULL }; rc = amp_init_run();
		a9ok = rc == 0 && violations == 0 && a9_opens == 1 && a9_closes == 1 && amp_a9_res.opened && amp_a9_res.a[0].locked && amp_a9_res.a[1].locked;
		CHECK(a9ok, "A9 good: run passes, opened once, closed once, PLL locked l,r");
		CHECK(violations == 0, why_v);
		for (int a = 0; a < 2; a++)
			for (int i = 0; i < 10; i++)
				if (amp[a].a9v[i] != a9_expect(A9R[i], a9v0[a][i])) {
					printf("  amp %d reg %#x = %#x want %#x\n", a, A9R[i], amp[a].a9v[i], a9_expect(A9R[i], a9v0[a][i]));
					CHECK(0, "A9 register = the stock field merge of its start value");
				}
		CHECK(amp[0].a9v[10] == a9v0[0][10] && amp[1].a9v[10] == a9v0[1][10] && !amp[0].a9_bad && !amp[1].a9_bad, "PWR_CTRL1 untouched");
		CHECK(amp[0].pll_en_seen && amp[1].pll_en_seen && has("L a9 pll locked") && has("R a9 pll locked"), "PLL lines");
		CHECK(amp_a9_res.a[0].polls == 1u && amp_a9_res.a[1].polls == 1u, "PLL poll stops at the first lock read (the model locks at once)");
		CHECK(restored_exact(), "A9: the same restore (resets low, every word back)");
		printf("ok: A9 good: %u / %u A9 writes, PLL locked l,r, AOC open + close once, restore exact\n", amp[0].a9_nwr, amp[1].a9_nwr);

		reset_hw(); pll_never = 1; amp_a9_cb = (struct amp_a9_cb){ 1, cb_open, cb_close, NULL, NULL, 0u, 0, NULL }; rc = amp_init_run();
		CHECK(rc == 0 && violations == 0 && !amp_a9_res.a[0].locked && !amp_a9_res.a[1].locked && amp_a9_res.a[0].polls == 100u &&
		      amp_a9_res.a[1].polls == 100u && a9_closes == 1 && has("a9 pll NOT locked") && restored_exact(), "PLL never locks: 100 polls each, closed, restored");
		printf("ok: A9 PLL never locks (bounded 100 x 1 ms per amp, AOC closed, restore exact)\n");

		reset_hw(); a9_open_ret = 0; amp_a9_cb = (struct amp_a9_cb){ 1, cb_open, cb_close, NULL, NULL, 0u, 0, NULL }; rc = amp_init_run();
		CHECK(rc == 0 && violations == 0 && !amp_a9_res.opened && amp_a9_res.a[0].polls == 0u && a9_closes == 1 && restored_exact(), "AOC open refused: no PLL poll, close called, restored");
		printf("ok: A9 AOC open refused (no PLL poll, close still called)\n");

		reset_hw(); amp[1].a9v[10] = 1u; amp_a9_cb = (struct amp_a9_cb){ 1, cb_open, cb_close, NULL, NULL, 0u, 0, NULL }; rc = amp_init_run();
		CHECK(violations == 0 && amp_a9_res.a[0].cfg && !amp_a9_res.a[1].cfg && amp[1].a9_nwr == 0 && a9_opens == 0 && restored_exact(), "GLOBAL_EN found set on R: no R write, no AOC open");
		printf("ok: A9 GLOBAL_EN found set: no write to that amp, no AOC open\n");

		reset_hw(); amp[0].st1 = 0x40u; amp_a9_cb = (struct amp_a9_cb){ 1, cb_open, cb_close, NULL, NULL, 0u, 0, NULL }; rc = amp_init_run();
		CHECK(violations == 0 && amp[0].a9_nwr == 0 && amp[1].a9_nwr == 0 && a9_opens == 0, "error bit (H8): no A9 write, no open");
		amp_a9_cb = (struct amp_a9_cb){ 0 };
		printf("ok: A9 amp error bit: no TDM config, no AOC open\n");
	}
	/* ---- patch 0379 (sweetaoc=5): joint L/R poll <= 500 x 1 ms with a tick per ms, st3 snapshots, close on every path ---- */
	{
		reset_hw(); tick_need = 20; tick_bad = 0; amp_a9_cb = (struct amp_a9_cb){ 1, cb_open5, cb_close, NULL, cb_tick, 500u, 0, NULL }; rc = amp_init_run();
		CHECK(rc == 0 && violations == 0 && a9_opens == 1 && a9_closes == 1 && !tick_bad && amp_a9_res.a[0].locked && amp_a9_res.a[1].locked,
		      "A9 play: locked l,r after the stream clocks, opened once, closed once, ticks inside");
		CHECK(a9_ticks == 20 && amp_a9_res.a[0].polls == 20u && amp_a9_res.a[1].polls == 20u, "joint poll stops at the lock (20 ticks, 20 polls each)");
		CHECK(!(amp_a9_res.a[0].snap[0] & 2u) && (amp_a9_res.a[0].snap[1] & 2u) == 0u && amp_a9_res.a[0].snap[2] == 0u, "snapshots @1 / @10 unlocked, @100 not reached");
		CHECK(has("L a9 st3 @1=") && has("R a9 pll locked") && restored_exact() && amp[0].a9v[10] == a9v0[0][10] && amp[1].a9v[10] == a9v0[1][10],
		      "st3 line, PLL lines, restore exact, PWR_CTRL1 untouched");
		printf("ok: A9 play: joint poll locks at tick 20, AOC closed once, restore exact\n");

		reset_hw(); tick_need = 0; tick_bad = 0; amp_a9_cb = (struct amp_a9_cb){ 1, cb_open5, cb_close, NULL, cb_tick, 500u, 0, NULL }; rc = amp_init_run();
		CHECK(rc == 0 && violations == 0 && !tick_bad && a9_ticks == 500 && amp_a9_res.a[0].polls == 500u && amp_a9_res.a[1].polls == 500u &&
		      !amp_a9_res.a[0].locked && a9_closes == 1 && has("a9 pll NOT locked") && restored_exact(), "never clocks: 500 joint polls, closed, restored");
		CHECK(amp_a9_res.a[1].snap[4] == amp_a9_res.a[1].st3 && amp_a9_res.a[1].snap[0] == amp_a9_res.a[1].st3, "snapshots @1 and @500 taken");
		printf("ok: A9 play: never clocks (bounded 500 x 1 ms joint, AOC closed, restore exact)\n");

		reset_hw(); tick_need = 0; tick_bad = 0; amp_a9_cb = (struct amp_a9_cb){ 1, cb_open5, cb_close, NULL, cb_tick, 9999u, 0, NULL }; rc = amp_init_run();
		CHECK(a9_ticks == 500 && amp_a9_res.a[0].polls == 500u && a9_closes == 1, "polls above 500 clamped to 500");
		printf("ok: A9 play: poll bound clamped to 500\n");

		reset_hw(); tick_need = 1; tick_bad = 0; a9_open_ret = 0; amp_a9_cb = (struct amp_a9_cb){ 1, cb_open5, cb_close, NULL, cb_tick, 500u, 0, NULL }; rc = amp_init_run();
		CHECK(rc == 0 && violations == 0 && a9_ticks == 0 && !amp_a9_res.opened && a9_closes == 1 && restored_exact(), "play open failed: no tick, close called");
		printf("ok: A9 play: open failed (no tick, close still called)\n");
		amp_a9_cb = (struct amp_a9_cb){ 0 };
	}
	dsp_tests();	/* patch 0381 */
	/* patch 0384: PWR_CTRL1 and the power words pass only while armed for exactly that pair; never a DSP / init pair through the arm */
	CHECK(!amp_write_ok(AMP_REG_PWR_CTRL1, 1u), "PWR_CTRL1 refused unarmed");
	amp_pwr_row.on = 1; amp_pwr_row.reg = AMP_REG_PWR_CTRL1; amp_pwr_row.val = 1u;
	CHECK(amp_write_ok(AMP_REG_PWR_CTRL1, 1u), "PWR_CTRL1 := 1 passes armed");
	CHECK(!amp_write_ok(AMP_REG_PWR_CTRL1, 3u), "PWR_CTRL1 other value refused armed");
	CHECK(!amp_write_ok(AMP_PWR_SPE, 1u), "another register refused while PWR_CTRL1 armed");
	amp_pwr_row.reg = 0x4C50u; amp_pwr_row.val = 0x99u;
	CHECK(!amp_write_ok(0x4C50u, 0x99u), "a non-power register refused even armed");
	amp_pwr_row.on = 0; amp_pwr_row.reg = amp_pwr_row.val = 0;
	CHECK(!amp_write_ok(AMP_REG_PWR_CTRL1, 1u) && !amp_write_ok(AMP_PWR_CTRL2, 1u), "refused again after the arm");
	if (fails) {
		printf("amp_xnu_host_test: %u FAILED\n", fails);
		return 1;
	}
	printf("PASS amp_xnu_host_test\n");
	return 0;
}

