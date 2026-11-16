VERDICT: SAFE

# Review round 2 (final): xnu patch 0159 sweet-wlan-msgbuf after the fixes for review 1

Reviewer: Claude Fable 5.2 (independent, worked alone, did not write the patch, no phone access, nothing run on the phone, no code changed). Date 2026-11-16.
Reviewed: `git diff 3337859..706748f` for `xnu/patches/0159-sweet-wlan-msgbuf.patch`, `shim/wlan_wr_host_test.c`, `tools/test_wlan_wr_mutants.py`, `docs/AUTOMATION.md`, `docs/research/51-wlan-msgbuf-host-dma.md`. Round 1: `docs/reviews/0159-wlan-msgbuf-review-fable-1.md` (ids F1-F9 below are its ids).
Line refs: `wr.h:N` = `bsd/dev/sweet_wlan_wr.h`, `glue:N` = `bsd/dev/sweet_wlan.c` in `~/pixelios-artifacts/localbuild/work/w8/xnu/`; `test` = `shim/wlan_wr_host_test.c`; `mut` = `tools/test_wlan_wr_mutants.py`; `note` = `docs/research/51-wlan-msgbuf-host-dma.md`.
Tags: **[obs]** I ran it and saw it, **[src]** read from source, **[guess]**.
The header of the patch chain at `706748f`, the header the shim build extracts and the patched tree's `sweet_wlan_wr.h` are byte-identical (`cmp`) [obs]. The delta does not touch `sweet_wlan.c`: `sw3_ptr`..`sw3_wr` are the same bytes as in round 1 (`cmp`) and are contained verbatim in the patched tree [obs].

Summary. Both required fixes (F1, F2) are in, at the right place (W18, before any store and before bus master), and do what round 1 asked. F3 and F4 are implemented without a new loop or a new bus access. The delta opens nothing new that I could find. Levels 3-7 are still unchanged. All round-1 scenarios give the round-1 result except the two that were meant to change (Z13 now passes, Z15 now stops at W18). The mutant suite, which the author did not rerun after re-pinning one pattern, passes in my full rerun (`Ran 9 tests in 2659.322s`, `OK`). What is left are low-severity notes and the residual risk of the step itself, which round 1 stated and the note now carries (note s.10).

## Findings

| id | sev | finding | required fix |
|----|-----|---------|--------------|
| F1 | closed | W18 prints ring_info words 5-12 and stops with MSG unless all are 0 (wr.h:2098-2106), directly after the 15 reads and before the guard fill, `swl_done(SWF_W18)` and every run F store. [obs] the author's case for each of the 8 words (no store, Command untouched) and my Z15: stop at row 18, 0 stores, 0 Command writes. Two mutants, both killed. | none |
| F2 | closed | W18 refuses a pool with `pool_pa + SWM_POOL_LEN > 1 << 36` (wr.h:423, 2079), after the 64-bit wrap term. [obs] the author's cases: one page above the limit refused with POOL and no store; ending exactly at 2^36 accepted (high word 0xF stored). Note s.10 corrects s.2 and s.4. | none |
| F3 | closed | An item whose type byte is 0 ends the lap without moving the read index (wr.h:2293-2295); it is flushed and read again on the next poll. Bounds unchanged: 200 polls, at most 64 items per poll. [obs] Z13 (index first, bytes one poll later): pass, 41 chip-RAM stores, nothing ignored. N1 (item stays 0 forever): `no completion ... has bytes`, stop TMO after 2.0 s of fake time, bus master off. N5 (a never-written slot in front of written items): stop TMO, bus master off. | none |
| F4 | closed | `swl_left_f()` (wr.h:1016-1028) runs at every stop (wr.h:1041) and prints `left after stop: host pool guard bytes changed N` when the bus-master tuple was done. It calls only the cache callback and reads host memory. [obs] N4: with no pool, with no callback, or before bus master it reads, stores, flushes and prints nothing; in the normal case one flush, no bus read, no store, one line. N3: guard damage together with a link drop during the wait: stop PRE, restore skipped, `bus_master=ON (restore not done)` and `guard bytes changed 1 (DMA suspect)` both printed. | none |
| F5 | accepted | Claims withdrawn in note s.10: no FAULT_STATUS re-read and no console re-read after run F. The diagnostic gap stays: an S2MPU fault or a firmware trap shows only as `no completion`. Not a safety matter. | none; see N-b |
| F6 | closed | The documented command fails on `bus_master=unknown`, `restore of group 16 skipped`, `panic\(cpu`, `Kernel trap at` (docs/AUTOMATION.md:650) [src; xrun not run by me]. | none |
| F7 | closed | U8 (root port Command, DBI 0x04) is in note s.10 as a guess. | none |
| F8 | closed | The three wrong EQUIVALENT labels are `None` with killing cases (short MAC answer; bus-master tuple without W21 by a direct call; missing sync callback). 15 mutants added (F1 x2, F2 x2, F3, F4 x2, ring-item address fields x2, four `swm_sync` removals, command compare, version filter). All killed or correctly labelled in my full run. | none |
| F9 | open, L | Unchanged by design: the glue's gate for chip RAM after the release is a count (96). Chip RAM only. | none for this run |
| N-a | L | The eight host index words are read once, in W18. A firmware that sets one later is not noticed ([obs] N2: word 5 set at the bus-master store, the run passes). This is not what F1 was about (a value that is already there at start), and no host check can bind a firmware that changes its own state, so I ask for nothing. A second read right before W22 would be cheap if run F is touched again. | none |
| N-b | L | `swl_left_f()` prints nothing when the bus-master store was made but not read back (`done` bit not set; the `bus_master=unknown` case). The fail-on list catches that case by its own line. The code comment at wr.h:2077 names the DHD macro as `DHD_SET_DMA_MASK 36`; the source has `DHD_PCIE_DMA_MASK_FOR_GS101 36`. If Command already reads 0x6 before W22 the stop still prints no `bus_master` line (round 1 F6, optional part; [obs] Z7 again 43 of 43). | none |

## Per-item verdicts

### Each fix matches its finding and opens nothing new: PASS

- The header delta is four places: three defines (wr.h:423-425), the F2 term and the F1 block in `swl_w18()`, the F3 `break`, and `swl_left_f()` with its call. Nothing else in the patch changed; the glue did not change [obs, diff and `cmp`].
- No new store, no new tuple, no new bus read. `SWT_N`, the tuple table, `swl_f_kind()`, `swl_wr()`, the restore table are unchanged [src].

### F1 runs before any store and before bus master: PASS

- Order in `swl_w18()`: flags, pool (with F2), shared/ring_info in RAM, the 15 reads, F1 (print, then stop on the first non-zero word), counts, region checks, guard fill, flush, `swl_done(SWF_W18)` (wr.h:2067-2134). The run F tuples need `SWF_W18` (wr.h:334-335), `SWT_BM_ON` needs W21. A stop in W18 has no armed restore group of run F [src].
- [obs] Z15 and the author's 8 cases: `nothing_f()` holds (0 chip-RAM stores after the release, 0 Command writes, 0 doorbells).

### F3 cannot loop without bound or lose a completion: PASS

- The `break` leaves only the inner `for` (wr.h:2289); the outer poll loop still counts to `SWM_POLLS` and delays 10 ms when nothing was got (wr.h:2277). Worst case per request: 200 polls x 64 items, as before.
- Not lost: the read index is not advanced and not stored for the type-0 item; items before it in the same lap are consumed and the index store happens once (`moved`). On the next poll the item is flushed (wr.h:2290) before its type is read again, so a stale cached zero cannot hide the arrived bytes [src].
- A firmware that never fills the item: timeout stop, restore, bus master off ([obs] N1, N5; through the real `sw3_wr()` G4: stop TMO, Command 0x0002).
- Side effect: round 1's Z4 (write index jumps to 63) no longer prints 62 "ignored" lines; it waits on the first empty slot and times out. Better.

### F4 does no bus access after a stop and cannot fault without a pool: PASS

- `swl_left_f()` returns unless `SWT_BM_ON` is done, `swm_pa(x, 0, SWM_POOL_LEN)` is non-zero (pool pointer, address and exact length) and the callback is not NULL; then one callback, `swm_guard(x, 0)` over `x->pool`, one print (wr.h:1016-1028) [src]. The glue callback bounds its range and returns without a pool (glue:349-357).
- [obs] N4 (direct calls), N3/N3b (link down: the fake's read count does not move in the function; the cache callback is not a bus access).
- At levels <= 7 the tuple is never done, so the function returns at its first test.

### Levels <= 7 unchanged: PASS

- [obs] my round-1 probe again (host test and header at `a370142` against `706748f`, real `sw3_wr()` in front of the fake bus, levels 3-7, both boards): outputs identical, 1330 lines, 0 glue refusals; the old side is byte-identical to round 1's.

### Round-1 requirement verdicts R1-R9 and items 10-15: unchanged, with R1 and R9 now without reservation

- R1: the only host addresses the firmware can find that the patch does not write are now checked to be 0 (F1), and the pool is inside the 36-bit bus range (F2).
- R2: [obs] again: 61 stores x {failed, dropped, bit flipped} through the real `sw3_wr()`: Command 0x0002 at the end of each, 0 refusals (G3); 1795 reads failed once each: bus master off in all (Z10); link down at each store: the bit is left in 34 of 122 cases, each reported as ON or unknown, never as off (Z8c; my checker again flags the one case where the drop comes at the final bus-master-off store, which goes through and ends with the bit clear).
- R9: the guard count is now printed on every stop after bus master (F4).
- Item 12 (addressing) is not changed by any fix: the residual risk is a device write into kernel memory outside the pool, by a mechanism nobody has observed (an unknown translation, or a firmware that uses an address it was not given). Run F executes before the root is mounted read-write. Outcome in that case: a panic, or silent damage to `userdata`, which is re-flashable. Note s.10 states this and the acceptance for this development phone. I agree with it; it is the user's risk to take and it is now written down.

### The one `nomatch` in the author's mutant run: acceptable only with a rerun, which I did

- The author's log ends `FAILED (failures=1)`, `mutants rc=1`: the 0147 mutant "STOP does not run the restore" no longer matched because `swl_left_f(x);` follows `swl_left(x);`. The re-pinned pattern (mut, last hunk of the delta) is the same mutation on the new text [src].
- A suite that last ended red is not a result to run a phone step on, however small the reason. I reran the whole suite at `706748f` under the heavy lock: `Ran 9 tests in 2659.322s`, `OK`, rc 0, so every pattern matches and every mutant is killed or labelled. I also applied the re-pinned mutation by hand: the host test fails with `RESULT: FAIL wlan_wr_host_test (1122 checks)`, the number the author reports [obs].

## Commands and results

All through `pixos-run.sh`; heavy ones under the heavy lock.

| Command | Result [obs] |
|---|---|
| `cd shim && PIXELIOS_HOST_TESTS=always ./build.sh --board oriole --avb --pstore-fast --usb-gadget --out out/rev0159b` | rc 0; `PASS wlan_wr_host_test (compile)`, `RESULT: PASS wlan_wr_host_test`, `PASS wlan_wr_host_test`; 302 PASS lines; images built |
| `gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror -Iout/rev0159b -o /tmp/claude-1000/t0159b wlan_wr_host_test.c && /tmp/claude-1000/t0159b` | `RESULT: PASS wlan_wr_host_test`, rc 0, no sanitizer report |
| `python3 -B tools/test_wlan_wr_mutants.py` (full; includes the audit, board, glue sweep and glue mutant tests) | `Ran 9 tests in 2659.322s`, `OK`, rc 0 (the author's last full run ended `FAILED (failures=1)` on one `nomatch`, re-pinned afterwards) |
| `tools/patch-numbers check` | `RESULT: PASS this tree's patch numbers do not overlap other branches or reservations` |
| `python3 -B tools/test_build_wlan_level.py` | `OK` |
| `strings -a ~/pixelios-artifacts/xnu-local-w8-706748f/kernel.development.sweet \| grep ...` | `SWEET WLAN: W18 ring_info host index buffers %#x ... (must be 0: indices stay in chip RAM)` and `SWEET WLAN: left after stop: host pool guard bytes changed %u%s` are in the kernel (sha256 starts `91693c159339431f`) |
| `cmp` extracted header / chain header / patched tree | identical |
| Probe R8 (levels 3-7 before/after, real `sw3_wr()`) | identical, 1330 lines, 0 refusals |
| Probe G (level 8 through the real `sw3_wr()`) | G1 rc 0, 0 refusals, 39 stores, `bm` 0; G2 no pool in the glue: stop at W22, no doorbell; G3 61 x 3 faults: 0 bad; G4 index moved every poll over empty items: stop TMO, Command 0x0002 |

Scenarios (scratch copy of the new host test with my fake knobs, ASan/UBSan build):

| id | scenario | result [obs] |
|---|---|---|
| Z1a-g, Z2a-c | ring_info pointers on each other / on the shared area; ring counts 0xFFFF, 1024 | as round 1: stop at W18, no store |
| Z3, Z5a/b, Z12 | wrong id, length 0 / 3, two completions | as round 1: stop DMA / MSG / DMA, bus master off |
| Z4 | write index jumps to 63 | stop TMO, bus master off, nothing "ignored" (F3) |
| Z6a/b | answer after 1.99 s / 2.01 s | pass / stop TMO |
| Z7 | Command 0x6 before W22 (43 store positions, and from boot) | as round 1: stop PRE, no Command store, no doorbell, no `bus_master` line |
| Z8a/b/c | link drop at the bus-master store and at each run F store | as round 1 (ON / unknown reported; 34 of 122 leave the bit, none reported as off) |
| Z9a/b | second stage-3 call | stop STATE at W1, 0 stores |
| Z10 | each of 1795 run F reads fails once | 1795 stops, bit clear in all |
| Z11 | each of 61 run F stores with one bit flipped | bit clear in all |
| Z13 | index before item bytes | **pass** (was: completion lost) |
| Z14 | bus-master tuple directly before W21 | `-SWW_E_PHASE`, no store |
| Z15 | host index words non-zero | **stop MSG at W18, 0 stores, Command untouched** (was: pass) |
| N1 | completion item stays type 0 forever | stop TMO at W24 after 2.0 s, `has bytes`, bus master off |
| N2 | host index word 0 at W18, non-zero from the bus-master store on | run passes (read once); note N-a |
| N3/N3b | guard damage plus link drop in the wait (reads answering / all-ones) | stop PRE, restore skipped, `bus_master=ON (restore not done)`, `guard bytes changed 1 (DMA suspect)` |
| N4 | `swl_left_f()` without pool / without callback / before bus master / normal | nothing at all in the first three; one flush and one line in the fourth; no bus access in any |
| N5 | a never-written slot in front of written items | stop TMO, bus master off |

## The run command for the first level-8 run

The command of docs/AUTOMATION.md:648-650 at `706748f` is right as written:

```
SWEET_USBLOG_PORT=64738 tools/xrun --kernel ~/pixelios-artifacts/xnu-local-w8-706748f/kernel.development.sweet --board oriole --no-camera --gui --rails --cpus 8 \
  --wlan-hold --wlan-level 8 --wlanfw ~/pixelios-artifacts/wlan-fw/wlanfw-oriole.bin --intent wlan \
  --expect 'SWEET WLAN: (stage 3 level 8 (done|stopped)|fw version)' --fail-on 'restore=failed|DMA suspect|bus_master=ON|bus_master=unknown|restore of group 16 skipped' --fail-on 'panic\(cpu' --fail-on 'Kernel trap at' --label o5-w7-l8 --restart 600,bootloader --lock-wait 3600
```

- The kernel must be the one built from `706748f` (sha256 starts `91693c159339431f`), nothing newer or older.
- PASS is only: `W24 device-to-host DMA works`, `fw version <text> mac xx:xx:xx:xx:<b4>:<b5>` and `stage 3 level 8 done msgbuf round trip ok, bus master off`, with none of the fail-on lines. `stage 3 level 8 stopped` satisfies the expect so that the run ends, but it is a recorded stop, not a pass.
- Lines to record in any case: `host pool 256 KiB at pa ...` (must be below 0x10_0000_0000), `W18 ring_info host index buffers ...` (eight zeros, or the stop), and after a stop `left after stop: bus_master=...` and `host pool guard bytes changed N`.
- A stop at W18 with non-zero host index words, or `no completion ... is untouched`, ends the ladder as the note says; no retry with changed code without a new review.
- After a run that panics outside the Wi-Fi code or shows `DMA suspect`: plan stop H10 counts it, and `userdata` is re-flashed before the image is trusted again.

## What I could not verify

- Anything about the real firmware: that the eight host index words read 0, that it then reads chip-RAM indices, that it ignores the upper half of an index slot, that host_cap 0x1407 is enough, that it completes requests with the interrupt mask closed.
- Device-to-host addressing on the phone: pass-through with no inbound region, the S2MPU with CTRL0 = 0, the root port's Command state (U1, U2, U8). The 36-bit width is from DHD source, not measured.
- `tools/xrun` with three `--fail-on` options was read, not run.
- That the kernel at `~/pixelios-artifacts/xnu-local-w8-706748f/` was built from exactly this tree: I checked the two new strings and its hash, not the build.
