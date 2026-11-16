VERDICT: SAFE

Run configuration: `--cpus 1` with `sweetmali=0x202`, kernel `xnu-local-lane-g-63266d1/kernel.development.sweet`, the command of
`docs/AUTOMATION.md:666-668`. The verdict covers that run and its `sweetmali=0x300` follow-up on one CPU. It does not cover a run with more
than one CPU (round-1 F5 is documented, not fixed). Nothing persistent is written; the worst case I could construct (N1 below, needs a Mali
mask register that does not work) is a hang ended by a power cycle.

# Review of xnu patch 0157 `sweet-mali-irq`, round 2 (final), reviewer fable 2

Reviewed 2026-11-16 at commit `63266d1` (branch `wip/oriole-gpu`): the delta `git diff dbe8b9d..63266d1 -- xnu shim tools`, the whole of
`xnu/patches/0157-sweet-mali-irq.patch` where the delta touches it, and the G4 section of `docs/AUTOMATION.md` (661-674). I did not write
the patch, did not change it and ran nothing on the phone. Round 1: `docs/reviews/0157-mali-irq-review-fable-1.md`.

Tags: **[obs]** = I ran it and saw the result, **[src]** = read from source, **[guess]** = not verified. Every GICv3 and Mali architecture
statement is [guess]: I had no copy of IHI 0069 or of the kbase sources to open in this session and cite them from memory.

Citations: `F:n` = `pexpert/arm/pe_fiq.c`, `S:n` = `osfmk/arm64/sleh.c`, `H:n` = `pexpert/arm/sweet_spi.h`, `M:n` = `bsd/dev/sweet_mali.c`,
all in `~/pixelios-artifacts/localbuild/work/gpu/xnu`. That tree matches the patch at 63266d1 [obs]: 0157 reverse-applies cleanly to the four
files, re-applying it gives byte-identical files, and `sweet_spi.h`, the `pe_fiq.c` hunk and the two handlers extracted from the patch text
are identical to the tree's.

## Findings

| id | sev | finding | required fix |
|---|---|---|---|
| N1 | L (residual) | The storm limit counts only "not mine" returns (H:172-177). A handler that returns "mine" on every entry is never limited. That needs `*_IRQ_MASK = 0` (M:319, M:338) to neither lower the line nor clear `*_IRQ_STATUS`, i.e. a mask register without effect. Model: 100000 entries in a cap of 100000, `storms=0` (S17b) [obs]. If only one of the two fails it is bounded: STATUS stays set but the line drops = 1 entry (S17a) [obs]; the line stays but STATUS reads 0 = the "not mine" path, 64 entries (S9) [obs]. `STATUS = RAWSTAT & MASK` and "line = STATUS != 0" are doc 35 U4, not observed on this phone (`docs/research/35-gpu-irq-path.md:45,137`). Why I accept it: the vendor driver on the same G78 depends on the same property in interrupt context (kbase masks the faulting AS in `MMU_IRQ_MASK` inside its hard handler and clears it later in a worker; `kbase_pm_disable_interrupts` writes the masks to 0) [guess, from memory]; and the consequence is a hang on a `fastboot boot` kernel, not a write. | None before the run. If wanted later: count every entry per slot and reset the count from `sm_thread`, so a storm of "mine" entries is limited too. The first run settles U4: `job` must equal the number of jobs (2 for the selftest). |
| N2 | L | The run command machine-checks only `mode=irq job>=2` and `irq attach failed` (`docs/AUTOMATION.md:667`). `selftest RESULT js0=PASS js1=PASS`, `unhandled_spi=0` and `storms=0` are pass criteria in the prose only (:673); xrun would score PASS with a failed selftest or with `storms=1`. `--fail-on 'panic\('` replaces xrun's default list, which also had `Kernel trap at` and `Assertion failed` (`tools/xrun:13-14`). | None for safety. Read the three values by hand before recording PASS, or add `--expect 'selftest RESULT js0=PASS js1=PASS' --fail-on 'storms=[1-9]' --fail-on 'unhandled_spi=[1-9]' --fail-on 'Kernel trap at'`. |
| N3 | info | "Consecutive" has no time limit: `notmine` (H:51) is reset only by a "mine" entry or a new attach (H:138, H:173). 64 isolated spurious entries on one SPI with no "mine" in between mask it for the rest of the boot. With correct wiring that cannot build up on 452 (every job gives a "mine") and 453 only fires on a real MMU fault. With JOB and MMU the other way round it happens after 64 jobs (S12) [obs]: IRQ mode quietly becomes the 5 ms poll, visible as `job=0 spurious=64 storms=1 last_unhandled=453`. Safe direction. | None. |
| N4 | info | After the storm path wrote `ICENABLER` (F:263-265) the interrupt can be taken once more if it was already forwarded to the CPU (no RWP wait in interrupt context) [guess]. That entry finds no handler, is counted in `unhandled_spi` and writes the same `ICENABLER` bit again. So `storms=1` can come with `unhandled_spi=1`; the handler is not called. | None. |
| N5 | info | A storm-masked SPI leaves `sm.irq_on = 1` (nothing tells the driver). That is correct, not a bug: submit still arms the masks (M:583-586), the line may rise at a disabled SPI, `finish()` zeroes them (M:237-240), the thread polls every 5 ms while busy (M:405-406). `irq counts mode=irq` is then printed although completion is by polling; `storms` shows it. | None. |
| N6 | info | If an interrupt of 452/453 ever arrived as FIQ, this kernel would not survive it: `sleh_fiq` reads no IAR and writes no EOI on SWEET, it calls `rtclock_intr` and returns (S:1994-2012), so a level FIQ would be re-taken without limit. I see no way to get there: see item 1. | None. |

Round-1 findings: F1 fixed, F2 fixed, F3 fixed, F4 comment path fixed (the glue is still not host-tested in the tree; my harness below
covers it), F5-F9 documented in the patch header (`0157-sweet-mali-irq.patch:15-20`), F8 taken into the run criteria (`AUTOMATION.md:673`).

## Per-item verdicts

**1. F1 fixed: PASS** [src + obs].
- Success condition: `sweet_spi_attach_ok(*isen, *igroupr, r.bit)` (F:334) = `(isen & bit) != 0` with `(void)igroupr` (H:155-160). The
  `IGROUPR |= bit` write is still there (F:325), the value is read back (F:333) and printed by the driver (M:368, M:371). Host test
  `shim/sweet_spi_host_test.c:84-87`; mutants "F1 require IGROUPR" and "F1 ignore ISENABLER" killed [obs].
- Model, oriole as F1 predicted it (IGROUPR/IGRPMODR RAZ/WI): attach 452 now returns 0 with ISENABLER14 = 0x10, IGROUPR14 = 0 (S6; round 1
  returned 5) [obs]. Write list unchanged: `[0x1b8] [0x2b8] [0x3b8] [0xb8] [0x138]` (S2) [obs].
- ISENABLER not sticking: attach calls `sweet_spi_detach` (ICENABLER, RWP wait, slot cleared; F:335, F:280-290) and returns 5, also when
  IGROUPR reads all-ones (S7, S7b: enable row 0, table empty) [obs]. The driver then detaches both INTIDs, sets `irq_on = 0`, zeroes the
  masks and prints `irq attach failed, polling as before` (M:373-377, M:346-355); `sm_thread` uses the 200 us poll (M:406) [src].
- The enable sequence once more, now that it is expected to succeed:
  - Group / IRQ or FIQ: with two security states a Non-secure write to `GICD_ISENABLERn` can only set the bit of a Group 1 Non-secure
    interrupt; for Group 0 and Secure Group 1 the bit is RAZ/WI to Non-secure, and `GICD_NSACRn` does not open the enable bits [guess].
    A Group 1 NS interrupt is signalled as IRQ while the PE is in Non-secure state, whatever its priority; the group decides IRQ/FIQ, not the
    priority [guess]. So "ISENABLER read back set" implies "arrives through `sleh_irq`", the same path the virtual timer and SGI 1 already
    use on this phone (S:1863-1900, F:167-178) [src]. `GICD_CTLR` read 0x12 in the probe = ARE_NS + Group 1 NS enabled
    (`device/google/oriole/docs/PORT.md:61`) [obs for the value, guess for the bit meaning]; `ICC_IGRPEN1_EL1 = 1` (F:214) [src].
  - Priority: `IPRIORITYR` is neither written nor printed (documented, patch header F9). The probe read 0 for 451-453 [obs, PORT.md:61].
    The Non-secure view of a Group 1 NS priority is the stored value shifted left by one, so 0 means stored 0x80 or lower: more urgent than
    the PPIs/SGIs, which this kernel sets to NS-view 0xA0 (F:175-177), and below `ICC_PMR_EL1 = 0xFF` (F:199) [guess for the view rule].
    Deliverable. A Non-secure read of the priority of a Secure interrupt also returns 0, so the probe's 0 says nothing about ownership;
    the ISENABLER read-back does.
  - Nesting: the Mali SPI being more urgent than the timer cannot preempt the timer handler or an IPI: `sleh_irq` runs with PSTATE.I
    masked from entry to return and never unmasks; `ICC_BPR1_EL1` (F:194) is therefore irrelevant. Order in the new branch is handler, then
    `ICC_EOIR1_EL1` with EOImode 0 (S:1890-1898, F:201-204) [src].
  - If it were taken as FIQ anyway: N6. Not reachable unless the ownership rule above is wrong; with firmware in EL3 a Group 0 interrupt
    would go to EL3, not to this kernel [guess].
  - Enabled but never delivered (`mode=irq job=0`): every job is finished by the 5 ms pass (M:405-406); the run fails its `--expect`. Safe.
  - `thread_wakeup` from this IRQ path and `sleh_interrupt_handler_prologue(state, DBG_INTR_TYPE_OTHER)` (S:1888) run for the first time on
    this port in this run [src]; both are stock XNU paths. First-run risk, not a finding.

**2. F2 fixed: PASS** [src + obs].
- Handlers: `STATUS == 0` writes `JOB_IRQ_MASK = 0` and `MMU_IRQ_MASK = 0`, counts `irq_spurious`, wakes the thread, returns 0 (M:312-317,
  M:331-336); `STATUS != 0` writes the own mask to 0, counts, wakes, returns 1 (M:319-322, M:338-341); not up returns 0 (M:309-311,
  M:328-330; never true after attach, and it would count towards the limit).
- Dispatch: return 1 resets `notmine` (H:172-175); return 0 increments it and the 64th in a row clears `fn`, counts `storms`, sets
  `last_unhandled` and returns `SSPI_MASK` (H:176-182); `sweet_spi_irq` then writes that SPI's `ICENABLER` bit (F:263-265). The handler is
  not called again: the slot's `fn` is NULL, later entries go the unhandled path (H:185-187). Host test
  `shim/sweet_spi_host_test.c:89-111`; four F2 mutants killed [obs].
- Return-value contract at every call site: the only call of a handler is H:172; the only registrations are M:367 and M:370; the type is
  `int (*)(void *)` in the table (H:49), `sweet_spi_set` (H:118), `sweet_spi_attach` (F:295-297) and the driver's extern (M:300). No other
  user of `sweet_spi_*` in the tree (grep: sweet_mali.c, sleh.c, pe_fiq.c, sweet_spi.h) [obs].
- Bounds (entries of `sleh_irq` for one SPI; "reset" = one "mine" entry, which needs a submit since that handler's last entry because the
  handler itself zeroed its mask):
  - one CPU, any storm: at most 64 handler calls per SPI, 128 for both, then masked. The thread cannot submit during a storm on one CPU, so
    at most one reset can fall into it: at most 65 per SPI in the worst ordering. Plus N4 (one or two entries without a handler call).
  - general: at most 64 x (jobs completed + 1) per SPI.
  - not bounded: N1 only.
- Scenarios [obs]: S9 (round-1 F2 case): 64 entries (round 1: 100000 of 100000), `[0x1b8]=0x10`, `storms=1`, 453 stays enabled. S13
  alternating: 704 entries for 10 jobs = 64 x 11. S14 both SPIs at once: 64 + 64, both masked, `storms=2`. S15a-c storm before, inside and
  interleaved with a detach: at most 64, the SPI ends disabled and the slot empty in every order. S16 storm on 452 inside the attach of 453:
  64 entries, attach of 453 still returns 0 and 453 dispatches to the MMU handler afterwards.
- Fallback after a storm mask or after the masks were zeroed by a "not mine" entry: `irq_on` stays 1 (N5), the thread's deadline while busy
  is 5 ms whatever the masks say (M:405-406), `check_job` reads RAWSTAT, which no handler writes (M:261). S18: 5 jobs after the storm mask,
  5 finished, 0 double, masks 0 when idle. S19/S14b: a "not mine" entry during a running job leaves both masks 0, the job is finished once
  by the deadline pass. S12 (lines swapped): 200 jobs, 200 finished, 0 double, 64 entries in total. A job cannot be finished twice:
  `finish()` is called only from `check_job` under the lock and clears `busy` (M:253, M:281, M:292, M:399-400); the handlers never call it.

**3. Second mask write from interrupt context: PASS** [src + obs].
- The handlers write only 0. The only non-zero mask writes are M:584-585, under `sm_lock`, followed without a failure path by `busy = 1`
  and START (M:587-588). So "mask non-zero while idle" needs no handler at all and cannot be produced by one. `finish()` (M:237-240),
  `sm_irq_off` (M:351-354), reset (M:655-656) and config (M:639-640) write 0 too: a handler write interleaved with any of them changes nothing.
- "Mask zero while a job runs": now possible on one CPU too (a "not mine" entry on either SPI between M:584 and completion zeroes both, so
  also an MMU fault then raises no interrupt). Covered: the 5 ms deadline depends on `busy && irq_on`, not on the masks, and `check_job`
  reads both RAWSTAT registers every pass (M:261-275). Cost: up to 5 ms latency, plus the unchanged 3 s / 0.5 s timeouts.
- Reset: the masks are 0 before `sm_reset` (finish zeroes them first), so only a foreign line can enter a handler during a reset; it then
  reads one STATUS register and writes two masks to 0 while the GPU resets. The driver itself polls GPU registers during the reset; the
  reset value of the masks is 0 [src for the driver, guess for the reset value].
- S20: 2,000,000 random steps of submit / completion / thread pass / interrupts / stale handler writes landing late / spurious takes:
  83264 submitted, 83264 finished, 0 double, mask non-zero while idle 0 times, mask zero while busy 836236 times (each ended by a pass) [obs].

**4. Without bit 0x200 nothing changes: PASS** [src].
The delta touches, outside code reached only from `mode & 0x200`: the handler signature in the table type and nothing else. `sleh.c` hunk
unchanged from round 1 (S:1850-1852, S:1890-1897). `sweet_spi_irq` still returns 0 after one load while the net is unarmed (F:256-258);
the new dispatch code is behind it. `sm_irq_attach` and `sm_irq_report` are called only at M:1119-1121 and M:1132-1134; `irq_on` is set
only at M:379; the `irq_on` tests in `finish`, `sm_thread` and submit are as in round 1. The delta does not touch the replay code or the
0155-F1 hunk (M:981-983), so the G2 modes (`sweetmali=0x100`, `=2`) are as round 1 item 8 described them. Not verified at instruction level.

**5. F3: PASS, with N2** [src + obs].
`--expect 'SWEET MALI: irq counts mode=irq job=([2-9]|[1-9][0-9])'`, `--fail-on 'irq attach failed'` (`AUTOMATION.md:667`); `mode=poll`
"is its own outcome (FAIL of this step, but safe)" (:673). Regex tried on sample lines: matches `mode=irq job=2`, `17`, `100`; does not
match `mode=irq job=1`, `mode=irq job=0`, `mode=poll job=0` [obs]. A `mode=poll` run cannot be scored PASS.

**6. Mutants: PASS** [obs].
22 of 22 killed: the 16 of round 1 by name, plus F1 require IGROUPR, F1 ignore ISENABLER, F2 no storm limit, F2 no reset on mine, F2 slot
kept after storm, F2 storms not counted. The delta of the mutant list and of the host test is additions only, except `h0`/`h1` returning
`int` (`git diff dbe8b9d..63266d1 -- tools shim`). Replay mutants: 49 killed, 0 survived, as before.

**7. Anything new:** N1-N6 above. Nothing that blocks the run. Also checked: the kernel artifact contains the new report string with
`storms=%u`, so it was built from a tree with this delta [obs]; the patch header describes the F5/F6/F7/F9 limits [src].

## Commands and results

Clean checkout `/tmp/claude-1000/review-0157b` (detached at 63266d1), through `pixos-run.sh` [obs]:

- `cd shim && flock ~/pixelios-artifacts/scratch/heavy.lock env PIXELIOS_HOST_TESTS=always ./build.sh --board oriole --avb --pstore-fast --usb-gadget --out out/rev0157b`: exit 0, images written, no FAIL line. Lines:
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
- `python3 -B tools/test_sweet_spi_mutants.py`: `Ran 2 tests ... OK`, 22 lines `killed`, none survived.
- `python3 -B tools/test_mali_replay_mutants.py`: 49 `killed`, 0 survived.
- `tools/patch-numbers check`: `RESULT: PASS this tree's patch numbers do not overlap other branches or reservations`, exit 0.
- `grep -a 'SWEET MALI: irq' ~/pixelios-artifacts/xnu-local-lane-g-63266d1/kernel.development.sweet`:
  ```
  SWEET MALI: irq attach INTID %u -> %d ISENABLER14=%#x IGROUPR14=%#x IROUTER=%#llx
  SWEET MALI: irq attach failed, polling as before
  SWEET MALI: irq mode on (JOB %u, MMU %u; GPU %u not attached)
  SWEET MALI: irq counts mode=%s job=%u mmu=%u spurious=%u unhandled_spi=%u storms=%u last_unhandled=%u
  ```

My harness [obs]: scratch, not in the tree (the round-1 one was gone, so it is rewritten). It includes verbatim, extracted from the patch
text at 63266d1: the `pe_fiq.c` hunk, `sweet_spi.h` and `sm_isr_job`/`sm_isr_mmu`. Mine: a GICD model (enable set/clear, IGROUPR/IGRPMODR
RAZ/WI, ignored ISENABLER, stuck RWP, level lines, abort on any write outside the five register blocks), a Mali mask model
(`STATUS = RAWSTAT & MASK`, line = STATUS != 0, both switchable), a `sleh_irq`-like loop with an entry cap, and a reduced thread model
(submit / pass / dead). `-Wall -Wextra`, ASan + UBSan. Output:

```
S1 attach/detach of 451 and other non-allowlisted INTIDs, NULL handler: EINVAL, 0 GICD writes, net not armed
S2 attach 452 (DS=1 model) -> 0, writes: [0x1b8]=0x10 [0x2b8]=0x10 [0x3b8]=0x10 [0xb8]=0x1f [0x138]=0x10
S2 double attach 452 -> 16, GICD writes 0, enable row 0x10
S3 detach, then a late take of 452: 1 entry, masked+counted (unhandled=1), job handler calls 0, row 0x20 (453 still enabled)
S4 unattached SPI 600 stuck high, net NOT armed: 1000 entries in a cap of 1000, 1000 through the old path, GICD writes 0
S4 same after one attach (net armed): 1 entry, masked by [0x1c8]=0x1000000  unhandled=1 last=600
S4b armed net: SGI/PPI/special/out-of-range INTIDs return 0, no GICD write
S5 IROUTER=other CPU / attaching CPU not CPU0 / IRM / Aff3 / edge / IGRPMODR: EPERM, 0 GICD writes, net not armed
S6 IGROUPR RAZ/WI (DS=0): attach 452 -> 0, ISENABLER14 read back 0x10, IGROUPR14 0, row 0x10, slot fn set  [round 1: -> 5]
S7 ISENABLER write ignored: -> 5, row 0, table empty, last writes: [0x1b8]=0x10 [0x2b8]=0x10 [0x3b8]=0x10 [0xb8]=0x10 [0x138]=0x10 [0x1b8]=0x10
S7b IGROUPR reads all-ones but ISENABLER does not stick: -> 5, nothing enabled
S8 RWP stuck: -> 60, row 0, table empty
S9 attached 452, a line the handler cannot see stays high: 64 entries in a cap of 100000 (round 1: 100000), handler calls 64, then [0x1b8]=0x10  storms=1 unhandled=0 last=452, row 0x20
S18 after the storm mask: 5 jobs submitted, 5 finished, double finishes 0, 0 further entries, masks 0 when idle, handler calls still 64
S18b dead after a storm: detach of both, row 0
S10 451 enabled behind the driver's back and asserted: 1 entry, last_unhandled=451, row 0x30
S11 normal: 300 jobs, job=300 mmu=0 spurious=0 storms=0, finished 300, double 0, row 0x30
S11b one late spurious take after every job, 300 jobs: spurious=300 storms=0 row 0x30 (count reset by each mine)
S12 JOB/MMU lines swapped, 200 jobs: 64 entries total (max 1 per job), job=0 mmu=0 spurious=64 storms=1 last=453 row 0x10, finished 200, double 0
S13 alternating (63 not-mine, 1 mine) x 10 jobs then no more jobs: 704 entries, job=10 spurious=694 storms=1 row 0x20; bound 64*(jobs+1)=704
S14 452 and 453 both stuck high: 128 entries (452: 64, 453: 64), writes [0x1b8]=0x10 [0x1b8]=0x20  storms=2 row 0
S14b same with a job in flight: 128 entries, masks 0 while the job runs, finished by the deadline pass: 1/1, double 0
S15a storm, then detach on the thread: 64 entries, storms=1, row 0x20, slot NULL
S15b line rises between detach's ICENABLER write and the clear: entries on 452 0 (masked at the GIC), row 0x20, slot NULL, writes [0x1b8]=0x10
S15c detach interleaved with a running storm (disable not yet effective): 22 entries until masked, unhandled=1 storms=0 row 0x20
S16 452 storms inside attach(453): 64 entries, attach(453) -> 0, storms=1, row 0x20, slots: [452 NULL] [453 set]
S16b afterwards 453 still dispatches to the MMU handler (mmu=1) and the job finishes by the deadline pass
S17a STATUS reads RAWSTAT (ignores MASK) but the line obeys MASK: 1 entry, job=1
S17b MASK = 0 does not drop the line and STATUS stays non-zero: 100000 entries in a cap of 100000, all 'mine' (job=100000), storms=0  (NOT bounded: finding N1)
S19 not-mine entry during a job: masks 0/0 while busy, no interrupt at completion, deadline pass finishes 1/1, double 0
S20 2000000 random steps: submitted 83264 finished 83264 double 0; mask non-zero while idle 0 times; mask zero while busy 836236 times (covered by the deadline pass); storms=2 row 0
harness: all asserts passed
```

S1-S10 are the round-1 scenarios; S6 and S9 are the two that changed (F1, F2). The three asked for: S13 (alternating), S14/S14b (two SPIs),
S15a-c (storm during detach); S12, S16-S20 are extra. In S15c the storm started with `notmine = 10`; in S20 the two storms come from the
spurious takes the test injects itself.

## Not verified

- Every GICv3 statement: RAZ/WI of IGROUPR/IGRPMODR and of the enable bit of a Secure interrupt for Non-secure accesses with DS = 0, that
  NSACR does not open the enable bits, the Non-secure view of priorities, Group 1 NS = IRQ in Non-secure state, RWP, level-pending
  behaviour after EOI and after ICENABLER. From memory, no spec opened. Item 1's "ISENABLER sticks = ours = IRQ" rests on them.
- `STATUS = RAWSTAT & MASK`, "line = STATUS != 0", and that INTID 452 is the JOB line and 453 the MMU line on the hardware (N1, N3). The
  kbase behaviour I cite for N1 is from memory; the kbase copy doc 35 names (`R/gpu-full/mali_kbase`) was not found on this machine.
- That `thread_wakeup` from this IRQ path and the `DBG_INTR_TYPE_OTHER` prologue behave on this build as in stock XNU; that no closed code
  installs `cdp->interrupt_handler`.
- The compiled code: no kernel build by me, nothing disassembled. I checked only that the artifact carries the new strings.
- Anything on hardware. The GIC and Mali models in the harness are mine; the thread model is a reduction of submit/check_job/finish, not
  the driver text (the handlers, the table and the `pe_fiq.c` glue are the patch text).
- More than one CPU: F5 stands; the storm counter and the `fn = NULL` in dispatch add two more unordered writes to the same table.
- The pk-gpu HEAD moved to `df7bcf0` while I worked; I reviewed 63266d1 in a separate checkout.
