/* Host test of the AOC pre-load probe (xnu patch 0161, bsd/dev/sweet_aoc.h) against a fake SoC: reads only, bounds, the AOC
 * power-domain gate, a note before and a done (value print) right after every block, no APM SRAM access. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sweet_aoc.h"

static uint32_t mem[SWA_NWIN][SWA_MMU_LEN / 4];
static const uint32_t len[SWA_NWIN] = { SWA_REQ_LEN, SWA_GSA_LEN, SWA_MMU_LEN, SWA_PMU_LEN };
static int unmapped = -1, bad, cur = -1;
static unsigned reads, wreads[SWA_NWIN], notes[SWA_NWIN], dones[SWA_NWIN], order_bad, done_reads[SWA_NWIN];

static void
fnote(void *c, int w)
{
	(void)c;
	if (w < 0 || w >= SWA_NWIN || notes[w] || wreads[w] || cur != -1) {
		order_bad++;    /* out of range, twice, after a read, or before the previous block's done */
		return;
	}
	notes[w]++;
	cur = w;
}

static void
fdone(void *c, int w, const struct swa_res *r)
{
	(void)c;
	(void)r;
	if (w != cur || dones[w]) {
		order_bad++;
	}
	dones[w]++;
	done_reads[w] = wreads[w];
	cur = -1;
}

static int
fr32(void *c, int w, uint32_t off, uint32_t *out)
{
	(void)c;
	reads++;
	if (w < 0 || w >= SWA_NWIN || (off & 3u) || off > len[w] - 4u) {
		bad++;
		return 0;
	}
	if (w != cur) {
		order_bad++;    /* a read outside its block's note..done */
	}
	wreads[w]++;
	if (w == unmapped) {
		return 0;
	}
	*out = mem[w][off / 4];
	return 1;
}

static void
setup(uint32_t pd)
{
	memset(mem, 0, sizeof(mem));
	mem[SWA_W_PMU][SWA_PMU_STAT / 4] = pd;
	mem[SWA_W_REQ][0x40 / 4] = 0x5;
	mem[SWA_W_GSA][0x2C / 4] = 0x1;
	mem[SWA_W_MMU][0x34 / 4] = ((8u << 11) | (1u << 4) | 0u) << 17;        /* 8.1.0 like stock dmesg:907 */
	reads = 0;
	memset(wreads, 0, sizeof wreads);
	memset(notes, 0, sizeof notes);
	memset(dones, 0, sizeof dones);
	memset(done_reads, 0, sizeof done_reads);
	order_bad = 0;
	bad = 0;
	cur = -1;
	unmapped = -1;
}

int
main(void)
{
	static struct swa_res r;
	struct swa_bus b = { fr32, fnote, fdone, NULL };
	int fails = 0, ok, w;

#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } } while (0)
	setup(1);
	ok = swa_probe(&b, &r);
	CHECK(ok == 1 && r.pd_on == 1, "pd on: pass");
	CHECK(r.req_st == 5 && r.gsa[2] == 1, "pd on: values");
	CHECK(r.mmu_maj == 8 && r.mmu_min == 1 && r.mmu_rev == 0, "pd on: version 8.1.0");
	CHECK(bad == 0 && reads == r.nread && order_bad == 0, "pd on: in bounds, note/read/done order");
	CHECK(wreads[SWA_W_PMU] == 2 && wreads[SWA_W_GSA] == 10 && wreads[SWA_W_REQ] == 2 && wreads[SWA_W_MMU] == 6, "pd on: counts");
	for (w = 0; w < SWA_NWIN; w++) {
		CHECK(dones[w] == 1 && done_reads[w] == wreads[w], "pd on: one done per block, after all its reads");
	}

	setup(0x2);     /* bit 0 clear: domain off */
	ok = swa_probe(&b, &r);
	CHECK(ok == 1 && r.pd_on == 0, "pd off: pass (gated blocks skipped)");
	CHECK(wreads[SWA_W_MMU] == 0 && wreads[SWA_W_REQ] == 0 && notes[SWA_W_MMU] == 0 && dones[SWA_W_REQ] == 0, "pd off: gated blocks untouched");
	CHECK(wreads[SWA_W_GSA] == 10 && dones[SWA_W_GSA] == 1 && order_bad == 0, "pd off: gsa read and printed");

	setup(1);
	unmapped = SWA_W_PMU;
	ok = swa_probe(&b, &r);
	CHECK(ok == 0 && r.pd_on == 0 && wreads[SWA_W_MMU] == 0 && wreads[SWA_W_REQ] == 0, "pmu unreadable: gate closed");
	setup(1);
	unmapped = SWA_W_MMU;
	ok = swa_probe(&b, &r);
	CHECK(ok == 0 && r.ok_mmu == 0 && dones[SWA_W_MMU] == 1, "sysmmu unreadable: fail, still printed");
	setup(1);
	mem[SWA_W_MMU][0x34 / 4] = 0;
	ok = swa_probe(&b, &r);
	CHECK(ok == 0, "sysmmu version not 8");
	{
		struct swa_bus nb = { fr32, fnote, NULL, NULL };
		setup(1);
		CHECK(swa_probe(&nb, &r) == 0 && reads == 0, "no done callback: refused, no read");
	}
	CHECK(swa_probe(NULL, &r) == 0, "null bus");
	printf("RESULT: %s aoc_pre_host_test (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
