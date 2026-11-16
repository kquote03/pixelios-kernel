/* Host unit test for the pure SPI table of xnu patch 0157 (pexpert/arm/sweet_spi.h, docs/research/35 s.6/s.8): allowlist (INTID 452/453 only,
 * never 451), GICD offsets/bits, the attach preconditions on the read-back state and the IRQ dispatch decision (handler or mask-and-count).
 * shim/build.sh extracts the header from the patch. Built with -Werror and with ASan/UBSan. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sweet_spi.h"

static int hits[2];
static int mine0 = 1;
static int h0(void *a) { (void)a; hits[0]++; return mine0; }
static int h1(void *a) { (void)a; hits[1]++; return 1; }

int
main(void)
{
	/* the register map of docs/research/35 s.6 */
	struct sweet_spi_regs r;
	assert(sweet_spi_regs(452, &r) == 0);
	assert(r.word_off == 14 * 4 && r.bit == (1u << 4));
	assert(SSPI_ISENABLER + r.word_off == 0x138 && SSPI_ICENABLER + r.word_off == 0x1B8 && SSPI_ICPENDR + r.word_off == 0x2B8);
	assert(SSPI_ICACTIVER + r.word_off == 0x3B8 && SSPI_IGROUPR + r.word_off == 0x0B8 && SSPI_IGRPMODR + r.word_off == 0xD38);
	assert(r.icfgr_off == 0xC70 && r.icfgr_shift == 8 && r.irouter_off == 0x6E20);
	assert(sweet_spi_regs(453, &r) == 0 && r.bit == (1u << 5) && r.icfgr_shift == 10 && r.irouter_off == 0x6E28);
	/* allowlist */
	for (uint32_t i = 0; i < 1100; i++) {
		int ok = sweet_spi_regs(i, &r) == 0;
		assert(ok == (i == 452 || i == 453));
	}

	/* attach preconditions on the state read back before any write: level-triggered, group-mode 0, routed to the attaching CPU */
	struct sweet_spi_state s = { .icfgr = 0, .igrpmodr = 0, .irouter = 0, .mpidr_aff = 0 };
	assert(sweet_spi_precheck(452, &s) == 0);
	s.icfgr = 2u << 8;                                     /* edge */
	assert(sweet_spi_precheck(452, &s) != 0);
	s.icfgr = 2u << 10;                                    /* edge, but for 453 only */
	assert(sweet_spi_precheck(452, &s) == 0 && sweet_spi_precheck(453, &s) != 0);
	s.icfgr = 0;
	s.igrpmodr = 1u << 4;
	assert(sweet_spi_precheck(452, &s) != 0 && sweet_spi_precheck(453, &s) == 0);
	s.igrpmodr = 0;
	s.irouter = 1ull << 31;                                /* any-CPU routing */
	assert(sweet_spi_precheck(452, &s) != 0);
	s.irouter = 0x100;                                     /* Aff1 = 1: another core */
	assert(sweet_spi_precheck(452, &s) != 0);
	s.mpidr_aff = 0x100;
	assert(sweet_spi_precheck(452, &s) == 0);
	s.irouter = 0x100000000ull;                            /* Aff3 differs */
	assert(sweet_spi_precheck(452, &s) != 0);
	assert(sweet_spi_precheck(451, &s) != 0);
	const struct sweet_spi_state clean = { 0, 0, 0, 0 };
	assert(sweet_spi_precheck(452, &clean) == 0 && sweet_spi_precheck(451, &clean) != 0 && sweet_spi_precheck(454, &clean) != 0);

	/* MPIDR -> IROUTER affinity: Aff3 [39:32] -> [39:32], Aff2.Aff1.Aff0 [23:0] -> [23:0], MT/U bits dropped */
	assert(sweet_spi_mpidr_aff(0x80000000ull) == 0);
	assert(sweet_spi_mpidr_aff(0x81000100ull) == 0x100);
	assert(sweet_spi_mpidr_aff(0x0000002580020301ull) == 0x0000002500020301ull);

	/* table + dispatch */
	static struct sweet_spi_table t;
	assert(sweet_spi_dispatch(&t, 452) == SSPI_MASK);      /* nothing attached: mask it */
	assert(t.unhandled == 1 && t.last_unhandled == 452);
	assert(sweet_spi_set(&t, 452, h0, NULL) == 0);
	assert(sweet_spi_set(&t, 452, h1, NULL) != 0);          /* already attached */
	assert(sweet_spi_set(&t, 451, h1, NULL) != 0);          /* not allowed */
	assert(sweet_spi_set(&t, 453, NULL, NULL) != 0);        /* no handler */
	assert(sweet_spi_set(&t, 453, h1, NULL) == 0);
	assert(sweet_spi_dispatch(&t, 452) == SSPI_HANDLED && hits[0] == 1);
	assert(sweet_spi_dispatch(&t, 453) == SSPI_HANDLED && hits[1] == 1);
	assert(sweet_spi_dispatch(&t, 451) == SSPI_MASK && t.unhandled == 2 && t.last_unhandled == 451);
	assert(sweet_spi_dispatch(&t, 600) == SSPI_MASK && t.unhandled == 3);
	assert(sweet_spi_dispatch(&t, 31) == SSPI_NOT_SPI);     /* PPI: not this path */
	assert(sweet_spi_dispatch(&t, 1020) == SSPI_NOT_SPI);
	assert(sweet_spi_dispatch(&t, 0xFFFFFFu) == SSPI_NOT_SPI);
	sweet_spi_clear(&t, 452);
	assert(sweet_spi_dispatch(&t, 452) == SSPI_MASK && hits[0] == 1);
	/* mask target of an arbitrary SPI: the ICENABLER word and bit (the net masks any SPI, not only allowlisted ones) */
	uint32_t off = 0, bit = 0;
	assert(sweet_spi_mask_reg(600, &off, &bit) == 0 && off == SSPI_ICENABLER + 4 * (600 / 32) && bit == (1u << (600 % 32)));
	assert(sweet_spi_mask_reg(31, &off, &bit) != 0 && sweet_spi_mask_reg(1020, &off, &bit) != 0);
	assert(sweet_spi_mask_reg(1019, &off, &bit) == 0 && off < 0x200);

	/* review 0157 F1: ownership = the ISENABLER bit sticks; IGROUPR is RAZ/WI for Non-secure on this GIC (DS = 0), so it is printed, not required */
	assert(sweet_spi_attach_ok(1u << 4, 0, 1u << 4) == 1);
	assert(sweet_spi_attach_ok(1u << 4, 1u << 4, 1u << 4) == 1);
	assert(sweet_spi_attach_ok(1u << 5, 0xFFFFFFFFu, 1u << 4) == 0);
	assert(sweet_spi_attach_ok(0, 0xFFFFFFFFu, 1u << 4) == 0);

	/* review 0157 F2: an attached handler that keeps answering "not mine" (STATUS 0 while the line stays high) is masked after SSPI_STORM_MAX
	 * consecutive entries; one "mine" resets the run */
	{
		static struct sweet_spi_table s2;
		assert(sweet_spi_set(&s2, 452, h0, NULL) == 0);
		mine0 = 0;
		for (unsigned i = 0; i + 1 < SSPI_STORM_MAX; i++) {
			assert(sweet_spi_dispatch(&s2, 452) == SSPI_HANDLED);
		}
		mine0 = 1;
		assert(sweet_spi_dispatch(&s2, 452) == SSPI_HANDLED);   /* resets */
		mine0 = 0;
		for (unsigned i = 0; i + 1 < SSPI_STORM_MAX; i++) {
			assert(sweet_spi_dispatch(&s2, 452) == SSPI_HANDLED);
		}
		assert(s2.storms == 0);
		assert(sweet_spi_dispatch(&s2, 452) == SSPI_MASK);      /* the SSPI_STORM_MAX-th in a row */
		assert(s2.storms == 1 && s2.last_unhandled == 452);
		assert(sweet_spi_dispatch(&s2, 452) == SSPI_MASK);      /* stays masked: the slot was cleared */
		mine0 = 1;
		int before = hits[0];
		assert(sweet_spi_dispatch(&s2, 452) == SSPI_MASK && hits[0] == before);   /* the detached handler never runs again */
	}

	printf("sweet_spi_host_test: PASS\n");
	return 0;
}
