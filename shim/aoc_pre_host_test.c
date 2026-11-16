/* Host test of the AOC pre-load probe (xnu patch 0161, bsd/dev/sweet_aoc.h) against a fake SoC: reads only, bounds, ch13 search, AOC power-domain gate and block notes. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sweet_aoc.h"

static uint32_t mem[SWA_NWIN][SWA_ACPM_LEN / 4];
static const uint32_t len[SWA_NWIN] = { SWA_REQ_LEN, SWA_GSA_LEN, SWA_MMU_LEN, SWA_ACPM_LEN, SWA_PMU_LEN };
static int unmapped = -1, bad;
static unsigned reads, wreads[SWA_NWIN], notes[SWA_NWIN], order_bad;

static void
fnote(void *c, int w)
{
	(void)c;
	if (w < 0 || w >= SWA_NWIN || notes[w] || wreads[w]) {
		order_bad++;    /* a note after a read, twice, or out of range */
		return;
	}
	notes[w]++;
}

static int
fr32(void *c, int w, uint32_t off, uint32_t *out)
{
	(void)c;
	reads++;
	if (w >= 0 && w < SWA_NWIN) {
		if (!notes[w]) {
			order_bad++;    /* a read of a block before its note */
		}
		wreads[w]++;
	}
	if (w < 0 || w >= SWA_NWIN || (off & 3u) || off > len[w] - 4u) {
		bad++;
		return 0;
	}
	if (w == unmapped) {
		return 0;
	}
	*out = mem[w][off / 4];
	return 1;
}

static void
setup(uint32_t nch, uint32_t ch13_at)
{
	uint32_t tab = 0x1000, i, d;

	memset(mem, 0, sizeof(mem));
	mem[SWA_W_PMU][SWA_PMU_STAT / 4] = 1;   /* AOC domain on */
	mem[SWA_W_REQ][0x40 / 4] = 0x5;
	mem[SWA_W_GSA][0x2C / 4] = 0x1;
	mem[SWA_W_MMU][0x34 / 4] = ((8u << 11) | (1u << 4) | 0u) << 17;        /* 8.1.0 like stock dmesg:907 */
	mem[SWA_W_ACPM][(SWA_ACPM_INIT + SWA_ACPM_APMAX) / 4] = nch;
	mem[SWA_W_ACPM][(SWA_ACPM_INIT + SWA_ACPM_TAB) / 4] = tab;
	for (i = 0; i < nch; i++) {
		d = (tab + i * SWA_ACPM_STRIDE) / 4;
		mem[SWA_W_ACPM][d] = (i == ch13_at) ? 13u : (i == 13u ? 99u : i);
		mem[SWA_W_ACPM][d + 4] = 0x3000 + 16 * i;
		mem[SWA_W_ACPM][d + 5] = 0x3004 + 16 * i;
		mem[SWA_W_ACPM][d + 10] = 0x3008 + 16 * i;
		mem[SWA_W_ACPM][d + 11] = 0x300C + 16 * i;
	}
	mem[SWA_W_ACPM][(0x3000 + 16 * 13) / 4] = 2;
	mem[SWA_W_ACPM][(0x3004 + 16 * 13) / 4] = 2;
	reads = 0;
	memset(wreads, 0, sizeof wreads);
	memset(notes, 0, sizeof notes);
	order_bad = 0;
	bad = 0;
	unmapped = -1;
}

int
main(void)
{
	static struct swa_res r;
	struct swa_bus b = { fr32, fnote, NULL };
	int fails = 0, ok;

#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } } while (0)
	setup(16, 13);
	ok = swa_probe(&b, &r);
	CHECK(ok == 1, "good: pass");
	CHECK(r.req_st == 5 && r.gsa[2] == 1, "good: values");
	CHECK(r.mmu_maj == 8 && r.mmu_min == 1 && r.mmu_rev == 0, "good: version 8.1.0");
	CHECK(r.acpm_idx == 13 && r.acpm_q[0] == 2 && r.acpm_q[1] == 2 && r.acpm_why == 0, "good: ch13");
	CHECK(bad == 0 && reads == r.nread, "good: in bounds");
	CHECK(order_bad == 0 && r.pd_on == 1 && wreads[SWA_W_MMU] == 6 && wreads[SWA_W_REQ] == 2, "good: notes before reads, gated blocks read");

	setup(16, 13);
	mem[SWA_W_PMU][SWA_PMU_STAT / 4] = 0x2;  /* bit 0 clear: domain off */
	ok = swa_probe(&b, &r);
	CHECK(ok == 1 && r.pd_on == 0 && wreads[SWA_W_MMU] == 0 && wreads[SWA_W_REQ] == 0 && notes[SWA_W_MMU] == 0, "pd off: sysmmu and aoc_req never read");
	CHECK(wreads[SWA_W_GSA] == 10 && r.ok_acpm == 1 && order_bad == 0, "pd off: gsa and acpm still read");
	CHECK(wreads[SWA_W_PMU] == 2 && r.nread == reads, "pd off: pmu read first, counts");
	setup(16, 13);
	unmapped = SWA_W_PMU;
	ok = swa_probe(&b, &r);
	CHECK(ok == 0 && r.pd_on == 0 && wreads[SWA_W_MMU] == 0 && wreads[SWA_W_REQ] == 0, "pmu unreadable: gate closed");
	{
		struct swa_bus nb = { fr32, NULL, NULL };
		setup(16, 13);
		CHECK(swa_probe(&nb, &r) == 0 && reads == 0, "no note callback: refused, no read");
	}

	setup(16, 99);
	ok = swa_probe(&b, &r);
	CHECK(ok == 0 && r.acpm_why == 4, "no ch13");
	setup(16, 3);
	ok = swa_probe(&b, &r);
	CHECK(ok == 0 && r.acpm_why == 5, "ch13 at another index");
	setup(40, 13);
	ok = swa_probe(&b, &r);
	CHECK(ok == 0 && r.acpm_why == 2, "ap_max too big");
	setup(16, 13);
	mem[SWA_W_ACPM][(SWA_ACPM_INIT + SWA_ACPM_TAB) / 4] = SWA_ACPM_LEN - 0x40;
	ok = swa_probe(&b, &r);
	CHECK(ok == 0 && r.acpm_why == 3 && bad == 0, "table past SRAM");
	setup(16, 13);
	mem[SWA_W_ACPM][(0x1000 + 13 * SWA_ACPM_STRIDE) / 4 + 10] = SWA_ACPM_LEN;
	ok = swa_probe(&b, &r);
	CHECK(ok == 0 && r.acpm_why == 6 && bad == 0, "queue pointer past SRAM");
	setup(16, 13);
	unmapped = SWA_W_MMU;
	ok = swa_probe(&b, &r);
	CHECK(ok == 0 && r.ok_mmu == 0 && r.ok_acpm == 1, "unmapped sysmmu");
	setup(16, 13);
	mem[SWA_W_MMU][0x34 / 4] = 0;
	ok = swa_probe(&b, &r);
	CHECK(ok == 0, "sysmmu version not 8");
	CHECK(swa_probe(NULL, &r) == 0, "null bus");
	printf("RESULT: %s aoc_pre_host_test (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
