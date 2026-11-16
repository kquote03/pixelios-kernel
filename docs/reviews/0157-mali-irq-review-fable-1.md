VERDICT: SAFE-AFTER-FIXES

Run configuration: the verdict does not depend on the CPU count, but the first run stays `--cpus 1` with `sweetmali=0x202` as planned (item 9).
As committed at dbe8b9d the patch cannot write anything persistent and a kernel that contains it behaves as before without bit 0x200; the two
fixes are for the run with the bit. F1: on this GIC the attach will most likely refuse itself (`-> 5`) and the run ends in `mode=poll`, safe
but without a result. F2: once F1 is fixed and IRQ mode really turns on, an interrupt whose handler reads STATUS == 0 is re-taken forever and
the net does not stop it. Fix both in one edit, then run.

# Review of xnu patch 0157 `sweet-mali-irq` (`sweetmali` bit 0x200), reviewer fable 1

Reviewed 2026-11-16 at commit `dbe8b9d` (branch `wip/oriole-gpu`): `xnu/patches/0157-sweet-mali-irq.patch` (whole patch),
`shim/sweet_spi_host_test.c`, `shim/build.sh:602-607`, `tools/test_sweet_spi_mutants.py`, `docs/AUTOMATION.md:661-674`. I did not write the
patch, did not change it and ran nothing on the phone. Builds and tests ran in a clean checkout of dbe8b9d (`/tmp/claude-1000/review-0157`).

Tags: **[obs]** = I ran it and saw the result, **[src]** = read from source, **[guess]** = not verified (this includes every GICv3
architecture statement: I had no copy of IHI 0069 to open and cite them from memory).

Citations: `F:n` = `pexpert/arm/pe_fiq.c`, `S:n` = `osfmk/arm64/sleh.c`, `H:n` = `pexpert/arm/sweet_spi.h`, `M:n` = `bsd/dev/sweet_mali.c`,
all in `~/pixelios-artifacts/localbuild/work/gpu/xnu`. That tree matches the stack at dbe8b9d for the five files 0155 and 0157 touch:
0157 reverse-applies cleanly, the result still reverse-checks against 0155, re-applying 0157 gives byte-identical files, and `sweet_spi.h`
equals the header extracted from the patch [obs].

## Findings

| id | sev | finding | required fix |
|---|---|---|---|
| F1 | M (function, fails safe) | The attach succeeds only if `GICD_IGROUPR14` reads back with the bit set (F:332-336). oriole has two security states with `GICD_CTLR.DS = 0` (TYPER 0x79041e bit 10 = 1, CTLR 0x12 bit 6 = 0, run gpu-oriole-g4b, `device/google/oriole/docs/PORT.md:61`). In that configuration `GICD_IGROUPRn` and `GICD_IGRPMODRn` are RAZ/WI for Non-secure accesses [guess, GICv3 spec from memory]; the probe read IGROUPR14 = 0 and the kernel's own note says the same of `GICR_IGROUPR0` ("reads as zero from the non-secure side even after this write; delivery still works", F:171-172). Then: write ignored, read-back 0, `sweet_spi_attach` detaches and returns 5, the driver prints `irq attach failed, polling as before`. My model of this case does exactly that (scenario S6) [obs]. The `IGRPMODR` precheck (H:95) is vacuous for the same reason. | Do not make the IGROUPR read-back a success condition; keep printing it. Keep the ISENABLER read-back (F:331): a Non-secure write cannot enable an SPI that is not Group 1 Non-secure [guess, spec], so that bit is the ownership test. Keep the `IGROUPR |= bit` write (needed where DS = 1, harmless where it is ignored). |
| F2 | M (safety) | The net covers only SPIs without a handler. An attached SPI whose handler reads `*_IRQ_STATUS == 0` only counts `irq_spurious` and returns (M:310-311, M:325-326); `sweet_spi_irq` returns 1, `sleh_irq` EOIs, and if the level line is still high the interrupt is taken again without limit. Model: 100000 entries in a cap of 100000, 0 GICD writes (scenario S9) [obs]. Reachable when the asserted line is not the one this handler reads: JOB and MMU INTIDs the other way round than the handlers (the 452 = JOB, 453 = MMU order comes from the DT and was never exercised on hardware), `STATUS = RAWSTAT & MASK` not holding (doc 35 U4, [guess]), or another source on INTID 452/453. On one CPU that is the hang review M2 described; `sm_thread` may or may not get enough time to reach `finish()`. As committed it is reachable only if F1's prediction is wrong. | Bound the spurious path before a run in which IRQ mode can turn on. Either: in the `STATUS == 0` branch write both `JOB_IRQ_MASK` and `MMU_IRQ_MASK` to 0 (the driver's own registers; submit re-arms them, the 5 ms deadline finishes the job), and/or let the handler return "not mine" so `sweet_spi_irq` masks the SPI with ICENABLER and counts it after N consecutive such returns. Add the case to the host test. |
| F3 | L | `docs/AUTOMATION.md:667` expects `'SWEET MALI: irq counts'`, which also matches `mode=poll` (attach refused, F1). xrun would report PASS for a run in which no interrupt was taken; the real criteria (`mode=irq`, job >= 2) are only in the prose at :672-673. | `--expect 'irq counts mode=irq job=([2-9]\|[1-9][0-9])'` and `--fail-on 'irq attach failed'` (or record `mode=poll` as a separate outcome). |
| F4 | L | Only `sweet_spi.h` is host-tested and mutated. The glue that does the hardware access is not: write order, "net armed before the first enable" (F:320), the read-back (F:331-336), detach, the `sleh.c` hook. I compiled the `pe_fiq.c` hunk verbatim against a GICD model without changing it (below), so it is testable as it is. `shim/sweet_spi_host_test.c:1` names the header `osfmk/arm64/sweet_spi.h`; it is `pexpert/arm/sweet_spi.h`. | Optional before the first run; wanted before a second driver uses `sweet_spi_attach`. |
| F5 | L | No barrier or atomic between the table stores (H:133-135), `sweet_spi_net = 1` (F:320) and the enable; `sweet_spi_dispatch` reads `fn`, then `intid`, then `arg` separately (H:157-160); `sweet_spi_clear` can empty a slot that another CPU is dispatching and a later `set` can reuse it. With two handlers, `arg` = NULL and both SPIs routed to the attaching CPU the worst outcomes are: the SPI is masked by the net (IRQ mode quietly becomes the 5 ms poll, visible as `unhandled_spi` != 0), or the other Mali handler runs once (it then reads STATUS 0, see F2). Not reachable with `--cpus 1`. | Before relying on more than one CPU: fixed slot per INTID (0 = 452, 1 = 453) and `os_atomic_store(..., release)` / `load(..., acquire)` for `fn` and `sweet_spi_net`. |
| F6 | L | `sm.dead` set by `sweet_mali_map`/`sweet_mali_unmap` (M:487, M:521) does not wake `sm_thread`; the GIC detach (M:392-394) then happens at its next pass, up to 1 s later when idle, 5 ms when busy. Both SPIs stay enabled in that window. The masks are 0 whenever no job runs, so the lines are low; no hazard found. | None needed; a `thread_wakeup(&sm.thread)` where `dead` is set would close the window. |
| F7 | info | The net stays armed for the rest of the boot after the first attach that passes the precheck, also when that attach ends in EIO/ETIMEDOUT or after detach (F:320, never reset). From then on any Non-secure SPI that fires without `sweet_spi_attach` is masked. Nothing in the tree depends on such an SPI today (item 5). | Document for the next driver that wants an SPI. |
| F8 | info | `spurious=0` is a pass criterion (`docs/AUTOMATION.md:673`). One spurious entry is legitimate when `finish()` zeroes the masks while the interrupt is already signalled, or if the line falls later than the EOI. | Treat a small non-zero `spurious` as a note, not a failure (after F2 it is bounded). |
| F9 | info | `IPRIORITYR` is neither written nor printed by the attach. The probe read priority 0 for 451-453 [obs, PORT.md:61], which passes `ICC_PMR = 0xFF` (F:199). | If a run shows `mode=irq job=0`, read the priority and `ISPENDR14` first. |

## Per-item verdicts

**1. Only 452 and 453 can be attached: PASS** [src + obs].
`sweet_spi_allowed` is `intid == 452u || intid == 453u` (H:55-58). Every path into the table or a per-INTID register goes through it:
`sweet_spi_regs` (H:64) at the top of `sweet_spi_attach` (F:304) and `sweet_spi_detach` (F:283), again in `sweet_spi_precheck` (H:88) and in
`sweet_spi_set` (H:120). The table is `static` in pe_fiq.c (F:232) and has no other writer. The comparison is on the full 32-bit value, so
`452 + 1024` or `0x100001C4` do not alias. The driver passes the constants 452 and 453 only (M:80-81, M:357, M:360); 451 appears only in a
printf (M:371). My run: attach of 451, 0, 31, 450, 454, 484, 1020, 1023, 1476, 0x100001C4, 0xFFFFFFFF and of 452 with a NULL handler all
return 22 with 0 GICD writes and the net unarmed; detach of 451 and 600 writes nothing (S1) [obs]. The one write that is not allowlisted is
the net's `ICENABLER` (disable only) for an unattached SPI 32..1019 (H:105-112, F:263-265); it can never enable anything.

**2. `sleh_irq` before any attach: PASS** [src].
The diff of `sleh.c` against the tree without 0157 is two hunks: an `extern` (S:1850-1852) and, in the last `else` branch,
`if (sweet_spi_irq(intid)) ; else if (cdp->interrupt_handler) ...` (S:1890-1897) in place of `if (cdp->interrupt_handler) ...` [obs, diff].
- Spurious 1020-1023 (S:1871-1873), the timer (S:1874-1880) and SGIs `intid < 16` (S:1881-1886): no changed line. Timer and IPI paths cost
  nothing extra at source level.
- Other PPIs and all SPIs: `sweet_spi_irq` loads `sweet_spi_net`, finds 0 and returns 0 (F:256-258; `gicd_base` is not even read because of
  the `||`). Cost: one call into another object file, one load, one compare, return. Then the old `cdp->interrupt_handler` test and the same
  `ICC_EOIR1_EL1` write and ISB (S:1898-1900): same order as before, handler first, then EOI (EOImode 0: priority drop and deactivate, F:201-204).
- There is no FIQ involvement: on this port everything is Group 1 and arrives through `sleh_irq` (S:1864-1867); `sleh_fiq` is untouched.
- In practice that branch does not run today at all: the only enabled PPI besides SGI 1 is the virtual timer (F:178), and the read-only probe
  found no non-zero `GICD_ISENABLER` word on oriole [obs, cited in `docs/reviews/0148-hsi2c-fg-review-fable-1.md:66`].
Not verified: the instruction-level cost (I did not disassemble a kernel) and that the compiler did not reorder the timer/SGI paths.

**3. Attach reads first and refuses without writing: PASS** [src + obs].
Reads at F:307-313: ICFGR28, IGRPMODR14, IROUTER (two 32-bit halves), MPIDR_EL1, plus IGROUPR14 and ISENABLER14 for the log line.
`sweet_spi_precheck` (H:84-101): edge bit set → refuse; IGRPMODR bit set → refuse; IRM (bit 31) set or `IROUTER & 0xFF00FFFFFF` different
from the attaching CPU's affinity → refuse. A refusal returns 1 at F:315 before the first `gicd_write32` (F:321) and before the net is armed
(F:320); the driver then prints `irq attach failed, polling as before` and `irq_on` stays 0 (M:363-367). My run: IROUTER = 0x100, attaching
CPU with Aff1 = 1, IRM, Aff3 ≠ 0, edge, IGRPMODR bit: each returns 1, 0 GICD writes, net unarmed (S5) [obs]. One side effect of a refusal:
`sm_irq_off` calls `sweet_spi_detach` for both INTIDs, which writes ICENABLER14 bits 4 and 5 (F:286) even though nothing was enabled. That is
a disable of the two allowlisted, already disabled SPIs; harmless, but "writes nothing" is not literally true for the driver path.

**4. The GICD writes: PASS for safety, F1 for function** [src + obs + guess].
- Write list from the model, good attach of 452: `[0x1b8]=0x10 [0x2b8]=0x10 [0x3b8]=0x10 [0xb8]=old|0x10 [0x138]=0x10` (S2) [obs] =
  ICENABLER14, ICPENDR14, ICACTIVER14, IGROUPR14, ISENABLER14, in the order of F:321-329. 453 is bit 5 (0x20). The model asserts on any write
  outside those five register blocks; none happened in any scenario.
- Arithmetic: 452 = 14 * 32 + 4, 453 = 14 * 32 + 5; word offset 0x38. ICFGR: 452 / 16 = 28 → 0xC70, field shift 8 (10 for 453), edge = the
  upper bit of the field. IROUTER 0x6000 + 8 * 452 = 0x6E20, 453 → 0x6E28. Matches doc 35 s.6 and `shim/sweet_spi_host_test.c:18-24`.
- Block bases IGROUPR 0x080, ISENABLER 0x100, ICENABLER 0x180, ICPENDR 0x280, ICACTIVER 0x380, ICFGR 0xC00, IGRPMODR 0xD00, IROUTER 0x6000,
  CTLR.RWP bit 31, IROUTER.IRM bit 31 with Aff3 in [39:32] and Aff2..0 in [23:0]: these are the GICv3 values as I know them [guess, spec not
  opened]; they agree with doc 35 s.6 and with the offsets the 0109 probe used on hardware.
- RWP waits are bounded at 1,000,000 reads of GICD_CTLR (F:235-246). A stuck RWP before the enable returns 60 with the SPI disabled and the
  table slot cleared (S8) [obs]. Read-back of ISENABLER/IGROUPR at F:331-332; not sticking → detach and 5 (S7) [obs].
- Writing IGROUPR from Non-secure EL1: where the distributor has one security state (DS = 1) the write is valid and needed. On oriole
  (DS = 0) it is RAZ/WI [guess]: the write is ignored, cannot move any interrupt between groups, and the read-modify-write cannot clear other
  bits either. Consequence is F1, not a hazard: the patch then disables the SPI again and polls. Ownership is decided by firmware; if 452/453
  were not Group 1 Non-secure the ISENABLER write would not stick either.
- ICFGR, IGRPMODR, IROUTER and IPRIORITYR are never written [src: the only `gicd_write32` sites added are F:264, F:286, F:321-324, F:329].

**5. The safety net: PASS for unattached SPIs, F2 for attached ones** [src + obs].
- Armed net, SPI without a handler: `sweet_spi_dispatch` counts it and returns `SSPI_MASK` (H:164-166), `sweet_spi_irq` writes that SPI's
  ICENABLER bit (F:263-265), `sleh_irq` EOIs. Model: SPI 600 stuck high is taken once, masked by `[0x1c8]=0x1000000`, `unhandled=1 last=600`;
  the same line before the net is armed is taken 1000 times in a cap of 1000 through the old path (the behaviour that exists today, S4) [obs].
- It cannot touch SGIs, PPIs, the special INTIDs or INTIDs >= 1024: 0, 1, 15, 16, 19, 27, 31, 1020-1024, 4096, 8192, 0xFFFFFF all return 0
  with no GICD write (S4b) [obs]; the timer and SGIs never reach the call (item 2).
- What it could mask that something needs: nothing found. Interrupts this kernel relies on: the virtual timer PPI (INTID 19 on the phone,
  `pexpert/pexpert/arm64/SWEET.h:50`) and SGI 1 for IPIs (`SWEET.h:51`), both enabled in the redistributor (F:178) and dispatched inline in
  `sleh_irq` (S:1874-1886). No SPI: no code in the tree writes a GICD enable (grep for `ISENABLER|ICENABLER|gicd_` hits only pe_fiq.c,
  sweet_spi.h, the two board headers and the read-only probe in sweet_mali.c); `ml_install_interrupt_handler` has one caller,
  `IOCPUInterruptController::enableCPUInterrupt` (`iokit/Kernel/IOCPU.cpp:627-634`), which nothing calls, so `cdp->interrupt_handler` is NULL
  [src grep; that no closed kext sets it is a guess]. Drivers are polled (`xnu/AGENTS.md`).
- An SPI with a handler that does not quiet its line is not covered: F2.

**6. Handlers: PASS, with F2** [src].
`sm_isr_job` / `sm_isr_mmu` (M:304-332): no lock, no printf, no sleep; read `JOB_IRQ_STATUS` (0x100C) / `MMU_IRQ_STATUS` (0x200C), write only
their own `*_IRQ_MASK` to 0, increment a counter, `thread_wakeup(&sm.thread)`. No `*_IRQ_CLEAR` write; `check_job` keeps all clearing, the
hard stop, the timeouts and the reset (M:258-294, unchanged by the patch). `thread_wakeup` goes to a global wait queue
(`osfmk/kern/sched_prim.c:1753`), which is interrupt-safe [src partly; guess that nothing on that path asserts under `MACH_ASSERT`: first run].
The `sm.up` test (M:307) is never false after attach (`sm.up` is set once at M:720 and never cleared), and nothing powers the GPU off afterwards.
Races:
- Job finishes between submit and mask arm: not possible; the masks are written (M:573-576) before `JS_COMMAND_NEXT = START` (M:578), and
  submit refuses unless `JOB_RAW == 0` and `MMU_RAW == 0` (M:558-562), so arming cannot raise the line by itself.
- Wakeup before the thread sleeps: the handler can fire after `check_job` read RAW (M:261) and before `lck_mtx_sleep_deadline` queues the
  thread (M:400). That wakeup is lost. RAWSTAT is kept (the handler only clears MASK), so the next pass sees it; the deadline is 5 ms while
  busy (M:396). Lost-wakeup cost: at most 5 ms per job, plus the unchanged 3 s / 0.5 s timeouts.
- Submit's own wakeup (M:580) is not lost: `sm_thread` holds `sm_lock` from its `busy` test to the sleep.
- Late interrupt after `finish()` zeroed the masks: the handler reads STATUS 0, counts a spurious, wakes the thread; the line is already low.
- More than one CPU only: the handler reads STATUS != 0, then the thread finishes that job by the 5 ms pass and a client submits the next,
  then the handler's `MASK = 0` lands on the new job's mask. That job gets no interrupt and is completed by the 5 ms pass. Bounded.

**7. Masks: PASS** [src].
Non-zero only from M:574-575 (submit, under the lock, immediately before START). Zeroed in `finish()` (M:237-240), the only place that clears
`busy` (M:253); in `sm_reset_do` (M:645-646) and `sm_hw_config_and_power` (M:629-630), both already there; in `sm_irq_off` (M:341-344) and
before attach (M:355-356). Dead device: `sm_thread` calls `sm_irq_off` (M:392-394), which detaches both SPIs from the GIC first (M:338-339)
and then writes the masks. When `dead` is set inside `finish()` this is the next statement of the same pass; from map/unmap it is delayed (F6).

**8. Bounded waits, behaviour without the bit: PASS** [src].
New waits: the RWP spin (bounded count), and the 5 ms sleep in place of 200 us while busy with IRQ mode on. No new unbounded loop.
Every new register access in the driver is behind `sm.irq_on` (M:237, M:392, M:396, M:573) or inside functions reached only from
`mode & 0x200` (M:1109-1111, M:1122-1124); `irq_on` is set only at M:369. `sweetmali=0x200` alone still returns at M:1101-1103.
What differs for `sweetmali=0x100` or `=2` on a kernel with 0157 but without the bit:
1. `sleh_irq`: the extra call for an INTID >= 16 that is not the timer (item 2); no such interrupt is enabled today.
2. `sm_replay_job`: a submit failure other than EINVAL now leaks the BOs instead of freeing them (M:974-976). Failure path only.
3. Four extra never-taken branches in `finish`, `sm_thread` and submit; a larger `sm` struct; different kernel layout and timing.
The G2 replay result (`gpu-o5-g2-replay1`) was on the kernel below this patch; none of the three changes a register write on the success path.

**Review 0155 F1: fixed** [src]. `sm_replay_job` sets `safe = 0` for a submit result other than EINVAL (M:972-977); `out:` then neither unmaps
nor frees and prints `leaked on purpose` (M:1016-1031), and the replay stops (M:1032-1034). The self-test already leaked for those results
(M:782-786).

**9. More than one CPU** [src + guess].
Safe with 8 CPUs, in the sense that nothing new can hang or corrupt:
- The precheck passes only if the attaching thread is on the CPU IROUTER names (IROUTER = 0 on oriole [obs] = CPU 0). On another CPU the
  attach returns 1 with zero GICD enables and the driver polls. Which CPU runs `sweet_mali_init` is not fixed, so with 8 CPUs the outcome
  (`mode=irq` or `mode=poll`) can differ from boot to boot.
- Handlers run on CPU 0, `sm_thread` anywhere. They share only Mali MASK writes of 0, RAWSTAT (never written by the handler) and the wakeup;
  the races are in item 6 and are bounded by the 5 ms deadline.
- `irq_job`, `irq_mmu`, `irq_spurious` are incremented only by the two handlers, both on CPU 0 and not nested: no lost updates. The thread
  reads them without ordering; they are diagnostics. `unhandled`/`last_unhandled` are plain increments and could lose counts if two CPUs took
  unattached SPIs at once; diagnostics again.
- A handler can still be running on CPU 0 after `sweet_spi_detach` returned on another CPU; it touches only the GPU registers and the wakeup,
  and the GPU stays powered.
Not safe to rely on with 8 CPUs: the table and `sweet_spi_net` have no publication barrier and no fixed slots (F5). The failure is a quietly
masked SPI or one wrong-handler call, not a hang, but it should be fixed before the multi-CPU run is used as a result. `--cpus 1` first.

**10. Worst case: PASS** [src].
The patch writes GIC distributor enable/pending/active/group bits and two Mali mask registers; all volatile, gone on a power cycle. No
storage, PMIC, clock or secure-world access is added. Worst case with the bit: an interrupt storm (F2) or a hang at boot, ended by a power
cycle; the kernel is started with `fastboot boot`, nothing is flashed. Whether xrun's `--restart 600,bootloader` gets a hung kernel back to
fastboot without the user holding the power key I did not check [guess: it may need the key].

## Commands and results

All in `/tmp/claude-1000/review-0157` (detached at dbe8b9d) through `pixos-run.sh` [obs]:

- `cd shim && flock ~/pixelios-artifacts/scratch/heavy.lock env PIXELIOS_HOST_TESTS=always ./build.sh --board oriole --avb --pstore-fast --usb-gadget --out out/rev0157`: exit 0, images written. Lines:
  ```
  PASS mali_pt_host_test (compile)
  mali_pt_host_test: PASS
  PASS mali_pt_host_test
  PASS mali_replay_host_test (compile)
  mali_replay_host_test: PASS
  PASS mali_replay_host_test
  PASS mali_replay_asan_host_test (compile)
  mali_replay_host_test: PASS
  PASS mali_replay_asan_host_test
  PASS sweet_spi_host_test (compile)
  sweet_spi_host_test: PASS
  PASS sweet_spi_host_test
  PASS sweet_spi_asan_host_test (compile)
  sweet_spi_host_test: PASS
  PASS sweet_spi_asan_host_test
  ```
- `python3 -B tools/test_sweet_spi_mutants.py`: `Ran 2 tests ... OK`, 16 of 16 mutants killed (allow 451, allow any, edge accepted, group
  modifier accepted, IRM accepted, other CPU accepted, precheck ignores allowlist, mask below 32, mask special ids, set without handler, set
  not allowlisted, set duplicate, dispatch PPIs, no unhandled count, no last unhandled, affinity keeps MT).
- `python3 -B tools/test_mali_replay_mutants.py`: `Ran 2 tests ... OK`, 49 killed, 0 survived.
- `tools/patch-numbers check`: `RESULT: PASS this tree's patch numbers do not overlap other branches or reservations`, exit 0.

My own scenarios [obs]: a scratch harness (not in the tree) that includes the `pe_fiq.c` hunk of the patch verbatim plus `sweet_spi.h`,
with `gicd_read32`/`gicd_write32` replaced by a small distributor model (enable set/clear, optional RAZ/WI of IGROUPR/IGRPMODR, optional
ignored ISENABLER, optional stuck RWP, level lines, a `sleh_irq`-like loop with an entry cap). Built with `-Wall -Wextra` and ASan/UBSan. Output:

```
S1 attach/detach of 451 and other non-allowlisted INTIDs: EINVAL, 0 GICD writes, net not armed
S2 attach 452 -> 0, writes: [0x1b8]=0x10 [0x2b8]=0x10 [0x3b8]=0x10 [0xb8]=0x1f [0x138]=0x10
S2 double attach 452 -> 16, GICD writes 0, enable row 0x10
S3 detach while pending: SPI disabled, a late take of 452 is masked+counted (unhandled=1), old handler not called; 453 still enabled (row 0x20)
S4 unattached SPI 600 stuck high, net NOT armed: 1000 entries in a cap of 1000, 1000 through the old path, GICD writes 0 (the pre-0157 behaviour)
S4 same after one attach (net armed): 1 entry, masked by [0x1c8]=0x1000000, unhandled=1 last=600
S4b armed net: INTID 0,1,15,16,19,27,31,1020..1024,4096,8192,0xFFFFFF return 0, no GICD write
S5 IROUTER=other CPU / attaching CPU not CPU0 / IRM / Aff3 / edge / IGRPMODR: EPERM, 0 GICD writes, net not armed
S6 IGROUPR RAZ/WI (two security states, DS=0): attach 452 -> 5, ISENABLER14 read back 0x10, IGROUPR14 0, row after 0, net=1, slot fn NULL
S7 ISENABLER write ignored: -> 5, table empty
S8 RWP stuck: -> 60 after a bounded wait, SPI never enabled, table empty
S9 attached SPI, handler returns without lowering the line: 100000 entries in a cap of 100000, handler calls 100000, GICD writes 0, unhandled=0 (NOT bounded by the net)
S10 451 enabled behind the driver's back and asserted: masked once (last_unhandled=451), 452/453 stay enabled (row 0x30)
```

S2 started with IGROUPR14 = 0xF: the write is `old | bit`, no other bit changes. S6 is F1, S9 is F2.

## Not verified

- Every GICv3 architecture statement (register layout, RAZ/WI of IGROUPR/IGRPMODR for Non-secure with DS = 0, that a Non-secure write cannot
  enable a Secure-group SPI, level-pending behaviour after EOI, RWP semantics): from memory, no spec opened. F1 is therefore a prediction;
  the two hardware observations it rests on (IGROUPR14 = 0 in the probe, the `GICR_IGROUPR0` note) are consistent with it but do not prove it.
- `STATUS = RAWSTAT & MASK` on the G78, and that INTID 452 is the JOB line and 453 the MMU line on the hardware (DT only).
- That `thread_wakeup` from this IRQ path passes all `MACH_ASSERT` checks of this build; that no closed code installs `cdp->interrupt_handler`.
- The compiled code: I ran no kernel build and disassembled nothing; the hot-path cost in item 2 is from the source.
- Anything on hardware. The model in my harness is mine, not a GIC.
- The worktree HEAD moved to `77ced29` while I worked; that commit does not touch the patch, the host test or the mutant tool
  (`git diff --stat dbe8b9d 77ced29` on those three files is empty). I reviewed dbe8b9d.
