VERDICT: SAFE-AFTER-FIXES

# Review: xnu patch 0150 sweet-wlan-fw-ready (sweetwlan=7), independent, different model family

Reviewer: Claude Fable 5.2 (delegate, worked alone, read-only, no phone access, no commit). Author family: Claude Opus. Date 2026-11-15.
Range `6b82d2b..4506d56` on `wip/oriole-next` (2 commits); `~/.hermes/cache/scratch/review/0150.diff` is byte-identical to `git diff 6b82d2b..4506d56` (`cmp`).
Line refs: `wr.h:N` = `bsd/dev/sweet_wlan_wr.h` after 0129-0132, 0147, 0150 (`final_header()`, byte-identical to the `shim/build.sh` chain); `glue:N` = `bsd/dev/sweet_wlan.c` from `glue_after()` at `4506d56` (chain with 0149, as the kernel gets it); `0150:N` = line of the patch file; `D/` = `~/pixelios-artifacts/research-src/dhd-bcm4389-bluejay-a13` (865527e); `O/` = `openbsd-src/sys/dev/pci` (9d8cbe8).
The branch tip is one commit past the range (`1614fda`, mutants glue chain without 0149); it is not part of this review.

Summary: the release sequence matches DHD, every store goes through the one writer, EP Command cannot be written (the glue refuses it), every chip-supplied pointer is bounds-checked, every wait is bounded. Nothing found can damage the phone or enable DMA. One real defect (F1): a stop in W17 can leave cfg 0x84 at 0x400000 with no restore attempt while the stop line reports `cfg84=0`. It is volatile and harmless to hardware, but it breaks requirement 4 and the "tag every claim" rule, and the fix is three lines. F2 is the test gap that hides the W15 restore. Fix F1 and F2, then run. F3-F8 are recommendations.

## Findings

| id | sev | finding | required fix |
|---|---|---|---|
| F1 | M | **A stop in W17 can leave cfg 0x84 = 0x400000, no restore is attempted, and the report says `cfg84=0`.** W16 ends with cfg 0x84 on the tail window (the last RAM word 0x49fffc is above 4 MiB) and calls `swl_done()` (wr.h:1572), which clears `x->armed` (wr.h:905) including `SWW_RG_W84`. W17 re-arms the group only if it moves the window again (wr.h:1508). When the shared area (and console) lie at or above 0x400000, no move happens, so a stop (read refused wr.h:1626/1639-1641, link lost before the second look wr.h:1663) latches with `armed_at_stop = 0`: `swl_restore()` skips the group and `swl_left()` prints `cfg84=0` (wr.h:760). Reproduced on the host (see "What I ran", probe A and B): `restore none live cfg84 0x400000 ... cfg84=0`. Also: with the link lost, the report should be `TAIL WINDOW (restore not done)` and `restore=skipped`, not `0` / `none`. | Required before the run. Either put the window back before W16 is marked done (`SWW_TRY(swl_w84_zero(x));` before `swl_done(x, SWF_W16)`; I checked that the whole host test still passes with it, mutant X20 below), or keep the group armed while `x->w84 != 0` (re-arm after `swl_done` in W16). Make `swl_left()` derive cfg84 from `x->w84` and `rs_ok`, not from `armed_at_stop` alone. Add a host case (shared, console and buffer all above 0x400000; read fault and link loss in W17) and a mutant. |
| F2 | M | **The W15 restore (the only restore this patch adds a path to) is not pinned by the tests.** `rune()` checks `x.restore == SWW_RS_NONE || (f.ca.ctl & 1u)` (shim/wlan_wr_host_test.c:2491). Removing `.rgrp = SWW_RG_CA` from all four `SWT_CAR_*` tuples survives (my mutant X1): a failed store after `RESET_CTL := 0` would then leave the CA7 running with IOCTL 0x3 and no restore, and the test passes. The shipped code is right (probe: faults at stores 2-5 give `restore ok`, `ctl 0x1`, `ca7=in reset`). | Required (tests only): for the faults at W15 stores 2..5 assert `x.restore == SWW_RS_OK && (f.ca.ctl & 1u) && (x.rs_ok & SWW_RG_CA)` and the `ca7=in reset` text; add the mutant "run E tuples do not arm the CA7 restore". |
| F3 | L | The glue has no independent gate for the release and no latch after it. `sw3_wr()` accepts IOCTL/RESET_CTL on the CA7 wrapper with any value at any level (glue:242-247) and BAR2 RAM stores at any time. So "CPUHALT only drops at level 7" and "no BAR2 store after the release" rest on the header alone (`.run = 5`, wr.h:797; exhausted tuples). W14 got a second, independent glue check for the same reason (review 0132 F5). | Recommended (required before the next level adds host_cap/ring stores): in `sw3_wr()` refuse a CA7-wrapper IOCTL value without 0x20 unless `h->l3 >= 7`; latch `released` on that store and refuse every `SWW_BAR2` store afterwards; extend the glue sweep harness. |
| F4 | L | `W17 done ... bus master untouched` (wr.h:1668) is printed without reading EP Command. `swl_wpre()` (which checks Command is MEM-only) runs only inside `swl_wr()`; W16/W17 use the RC-local `swl_lpre()`, and when the last reads were in window 0 no `swl_wr()` runs at the end. The code never writes Command (PASS, s.7), but the log line is a claim, not an observation. | Recommended: read cfg 0x04 at the end of W17 (the read is already in `swl_rt[]`), print it, and stop with `SWW_E_PRE` if `swl_pre_ep` fails; or drop the words from the log line. |
| F5 | L | Citations that do not match the pinned sources (values are fine): (a) "DHD 5 s (dhd_pcie.c:13636-13647)": the code comment says 5 s but `MAX_READ_TIMEOUT` is 2 s in this build (`D/dhd_pcie.c:594-598`, the 5 s branch is `NDIS`); the 5 s bound is bwfm/brcmfmac's (`O/if_bwfm_pci.c:869-875`, `linux-v6.18 .../brcmfmac/pcie.c:117, 1759-1766`). (b) `PCIE_SHARED_VERSION_5.._9` are `D/include/bcmpcie.h:45-49`, not 43-47. (c) bwfm range rule is `if_bwfm_pci.c:880-881`, not 876-877; the poll loop is 869-875. (d) `rings_info_ptr` at 0x30 is also in DHD (`bcmpcie.h:384`), cite it next to bwfm. (e) Plan W5 says "<= 5 s total"; the loop is 100 delays of 50 ms plus the read time of 100 polls (about 5.0-5.1 s; the host test accepts up to 5.1 s). | Fix the comment text in the patch header and at wr.h:1459, 1466. No code change. |
| F6 | L | Guards of run E that no test kills (my extra mutants, all SURVIVED): X18 the 120-character line cap of `swl_con_out()` (wr.h:1595; the guard of the 121-byte stack buffer wr.h:1580; no test has a line longer than 120; I checked the shipped code under ASan with a 1499-character line: longest payload 120, no report); X15 console pointer alignment (wr.h:1634; `swl_bp_rd` still refuses, but with a stop instead of "console not read"); X8 `.oval = 0x21` of `SWT_CAR_IOCTL_MID`; X9 `.need = SWF_W15` of `SWT_W84_ZERO_E`; X3 the token/RAM pre-check of W16 (wr.h:1540); X2 the disarm of `SWW_RG_W84` on window 0 (wr.h:1511, 1525); X11 the explicit `swl_w84_zero` in the not-up path (wr.h:1558; the restore covers it). | Add a long-line case and mutant (memory safety of a kernel stack buffer deserves one). List X2, X3, X8, X9, X11 as EQUIVALENT with the implying clause or add cases. |
| F7 | L | The patch header does not say what a stop in W16/W17 leaves: the CA7 stays released and the firmware keeps running (nothing is armed for `SWW_RG_CA` after `swl_done(SWF_W15)`); only `swl_left()` says so (`ca7=RELEASED (firmware running)`, wr.h:758). This matches DHD (it does not re-halt after a failed readshared) and is acceptable: the chip is reset by the next power cycle. | State it in the patch header and in the run notes. No code change. |
| F8 | L | `logs/runs` holds oriole runs for levels 1-4 only (the level 4 run stopped at W8 before 0147). Level 7 runs rows W8-W14 for the first time on this phone in the same boot if 5 and 6 are skipped. The code is safe either way (each row's gates are in `swl_wr`), but a stop is easier to read one level at a time. | Run `sweetwlan=5` and `6` on oriole and record them before `sweetwlan=7`. |

## Requirements

### 1. One deny-by-default writer: PASS
- The only store statement is still `*p = v` in `sw3_wr()` (glue:255), reached only through `swl_store()` (wr.h:689-693), called only by `swl_wr()` (wr.h:857) and `swl_restore()` (wr.h:733, 739). The patch adds no store statement and no `swl_rs[]` row; the glue hunk changes one log string and `level > 6` to `level > 7` (0150:28-41).
- New store call sites, all `swl_wr()`:
  - W15 `swl_win(x, SWW_WIN_CA7WRAP)` wr.h:1482 (existing tuple `SWT_WIN_CA7`, cfg 0x80).
  - W15 `swl_core_reset(x, SWW_WIN_CA7WRAP, 0x3, 0x1)` wr.h:1490 -> the four stores of wr.h:1024, 1028, 1033, 1036.
  - `swl_bp_rd()` wr.h:1508: cfg 0x84 := 0 or 0x400000 when the word needs the other window.
  - `swl_w84_zero()` wr.h:1523: cfg 0x84 := 0 (called at wr.h:1558, 1636, 1645, 1667).
  - W16 not-up path `swl_win(x, SWW_WIN_CA7WRAP)` wr.h:1558 (cfg 0x80, existing tuple).
  - Restore (existing rows): `SWW_RG_CA` RESET_CTL := 1 on the CA7 wrapper, `SWW_RG_W84` cfg 0x84 := 0 (wr.h:307-312).
- New tuples (6, wr.h:265-278; `SWT_N` 28 -> 34):

| tuple | space/window/offset | value | old / companion | flags, need |
|---|---|---|---|---|
| `SWT_CAR_ASSERT` | BAR0 CA7 wrapper RESET_CTL | 1 | old bit0 = 0; IOCTL & 0x3FFF = 0x21 | ONCE, run 5, `SWF_W14`, arms `RG_CA` |
| `SWT_CAR_IOCTL_MID` | BAR0 CA7 wrapper IOCTL | 0x3 | old = 0x21; RESET_CTL bit0 = 1 | ONCE, pred ASSERT, run 5, `SWF_W14` |
| `SWT_CAR_RELEASE` | BAR0 CA7 wrapper RESET_CTL | 0 | IOCTL = 0x3 | ONCE, pred MID, run 5, `SWF_W14` |
| `SWT_CAR_IOCTL_END` | BAR0 CA7 wrapper IOCTL | 0x1 | old = 0x3; RESET_CTL bit0 = 0 | ONCE, pred RELEASE, run 5, `SWF_W14` |
| `SWT_W84_TAIL_E` | cfg 0x84 | 0x400000 | old = 0 | reusable, TAIL, run 5, `SWF_W15`, arms `RG_W84` |
| `SWT_W84_ZERO_E` | cfg 0x84 | 0 | old = 0x400000 | reusable, TAIL, run 5, `SWF_W15` |

- The glue passes these without change: BAR0 IOCTL/RESET_CTL on the CA7 wrapper and cfg 0x84 as 0 or 0x400000 were already allowed (glue:242-247). See F3.

### 2. Constants cited: PASS (F5: citation text)
Opened and checked:
- Release sequence: `D/dhd_pcie.c:10343-10344` (`si_core_reset(bus->sih, 0, 0)`, the last statement of the exit path), current core = CA7 (`si_setcore(ARMCA7_CORE_ID)` at :10176 for the whole exit branch and again at :10200). `D/aiutils.c:1328-1347` -> `_ai_core_reset` :1242-1325: wait RESET_ST; `resetctrl := AIRC_RESET`; 10 us; wait; `ioctrl := bits|resetbits|SICF_FGC|SICF_CLOCK_EN` = 0x3; wait; loop `resetctrl := 0`; `ioctrl := bits|SICF_CLOCK_EN` = 0x1; 1 us. `SICF_FGC 0x2`, `SICF_CLOCK_EN 0x1` (`D/include/hndsoc.h:266-267`), `SICF_CPUHALT 0x20` (`D/include/siutils.h:306`). `UCM_CORRUPTION_WAR` appears only in `aiutils.c` (not in Makefile/Kbuild/Kconfig): not defined, as the patch says. `swl_core_reset(…, 0x3, 0x1)` (wr.h:1019-1049) is that sequence with the same order and waits; it is the function hardware already ran for SYS_MEM (W4) with the same values.
- cfg 0x84 values: `DHD_BUS_BAR1_BPWIN` `D/dhd_pcie.c:6149-6150`, used :6227; `PCI_BAR1_WIN 0x84` `D/include/pcicfg.h:489`. Same values as W8/W11.
- Poll: 100 x 50 ms `O/if_bwfm_pci.c:869-875`; brcmfmac `pcie.c:117, 1759-1766`; DHD loop and validity rules `D/dhd_pcie.c:13639-13647, 13652, 13698-13705`.
- Shared area: `D/include/bcmpcie.h:358-384`: flags 0x00, console_addr 0x14 (:366), fwid 0x1C (:370), rings_info_ptr 0x30 (:384); `O/if_bwfm_pci.h:123, 137`. Version mask 0xFF (`bcmpcie.h:54`), versions 5..9 (:45-49).
- Console: `D/include/hnd_cons.h:60-92` with `BCMDONGLEHOST`: vcons_in 0, vcons_out 4, log.buf 8, buf_size 0xC, idx 0x10; `O/if_bwfm_pci.h:156-158`; ring read `O/if_bwfm_pci.c:2319-2338`.
- Not traceable to a source (own choices, harmless): `SWW_SH_WORDS 16`, `SWW_CON_MAX 2048` (plan W5), `SWW_CON_LINE 120`, `SWW_CON_SETTLE_US 2 s`. `SWL_AGENT_IOCTL/RESET_CTL/RESET_ST` and `SWW_WIN_CA7WRAP 0x18120000` are inherited (0124/0129; the CA7 wrapper is in the oriole run log `wlan-oriole-l4`: `core 0x847 rev 9 ... wrap 0x18120000`).

### 3. Loops and waits bounded: PASS
- W15: `swl_wait` 30 x 10 us (five times), 10 x 10 us, single reads; fixed delays 10 us and 1 us (wr.h:1019-1049).
- W16: `for (i = 0; i < SWW_FWUP_POLLS; i++)` wr.h:1543: at most 100 polls, 50 ms apart, about 5.0-5.1 s; each poll is 16 RC-local reads, a cfg 0x84 read and one BAR2 read.
- Console: `swl_con_out` loops `n` times (wr.h:1586); first look `n <= 2048` (wr.h:1648), second look `n <= 2048 - con_bytes` (wr.h:1655); at most 2048 bytes and about 515 word reads in all, plus one `swl_wr` per window crossing (at most 3 per call).
- Second look: one fixed 2 s delay (wr.h:1650), then one index read.
- Worst case added to boot: about 7.1 s plus log output. `sw3_delay` is `delay()` (glue:282-286).

### 4. Stop paths, restore, report: FINDING F1 (and F2 for the test)
What each stop does (checked by reading and by the host probe):
- W15 pre-check not 0x21 / in reset: nothing armed, nothing stored, CA7 stays halted (wr.h:1487-1489).
- Stop inside W15 after the first store attempt: `SWW_RG_CA` armed (wr.h:856) -> `RESET_CTL := 1` stored, read back and printed (`restore space 3 win 0x18120000 off 0x800 := 0x1 ok`), gated by `swl_wpre`; report `ca7=in reset`. Probe: faults at stores 2-5 all end `restore ok`, `ctl 0x1`.
- W16 link lost: no TLP; cfg 0x84 restore skipped and reported (`restore=skipped`, `TAIL WINDOW (restore not done)`); CA7 running.
- W16 not up (token or 0 after 100 polls): cfg 0x84 := 0 through `swl_wr` (read back), CA7 wrapper logged as DHD does, CA7 running, `ca7=RELEASED (firmware running)`.
- W16 all-ones / pointer outside RAM: cfg 0x84 restored through `SWW_RG_W84` (armed since the first tail move), read back, reported.
- W17: **F1** when no window move happened in W17.
Left changed after a stop in W16/W17 and after success (by design): CA7 running (IOCTL 0x1, out of reset), SYS_MEM out of reset, ATU region 2 enabled, the image/NVRAM/rTLV/vector in chip RAM, cfg 0x80 on the last window selected; cfg 0x84 = 0 after success (wr.h:1667, tested by `left_ok`). No restore exists for a completed release; a power cycle resets the chip.

### 5. Default off: PASS
- No boot-arg or `sweetwlan=0`: return before any access (glue:380-382, unchanged). `sweetwlan >= 8`: "not touched" (glue:383).
- Level <= 6: run E is behind `x->level >= SWW_LEVEL0 + 5` (wr.h:1729); the six tuples have `.run = 5` and are refused by wr.h:797; `swl_wr_pick` does not hand a consumed tuple over to a run 5 tuple at a lower level (wr.h:613), so a repeated CA7 `RESET_CTL := 1` or cfg 0x84 write still ends `REPEAT` as before (host test: `-SWW_E_REPEAT`, no store). Level 7 with a PCIe core rev that W14 cannot handle still drops to level 5 (glue:447-450).
- `x->done` 32 -> 64 bit: every use is `SWT_BIT(t)`; for t < 28 the values equal `1u << t`; no decision changes. The host test of levels 3-6 passes unchanged and the level 6 case in `rune()` shows no W15 line and `ioctl 0x21`.
- Only visible differences below level 7: the stop code for a (never issued) CA7 `IOCTL := 0x3/0x1` is `level` instead of `no tuple`, and `tuples %#llx` in the stop line.

### 6. Chip-supplied pointers, sizes, indexes: PASS
- Every RAM read of run E goes through `swl_bp_rd()` (wr.h:1498-1516): word aligned, `a >= 0x200000`, `a - 0x200000 <= ram_bytes - 4` (with `ram_bytes >= 4` checked first), then the read allowlist (`swl_rt[]` run 2 rows) and the glue (`off < 4 MiB`, live window compare, glue:209, 224). No pointer is formed from a chip value; they are offsets into the BAR2 mapping.
- Shared pointer: aligned, `>= RAM base`, `<= last - 0x40` (wr.h:1567); all-ones, 0 and the token are stops before it.
- Console pointer: aligned, in RAM, `<= last - 0x10` (wr.h:1634). Buffer: `buf >= base`, `buf <= last`, `size != 0`, `size <= last - buf`, `idx < size` (wr.h:1643); the second index is checked again (wr.h:1653). The values are cached in `x`, so a later change in chip RAM cannot move the reads.
- Arithmetic: `last >= 0x200000 + 1791299 - 4` (W5 bound, re-checked wr.h:1540), so `last - 0x40`, `last - 0x10`, `last - buf` do not underflow; `e + size - n` (wr.h:1585) has `n <= e` (first look) or `n < size` (second look) and `e, size < 4 MiB`, no wrap; `SWW_CON_MAX - con_bytes >= 0`; `% con_size` only with `size != 0`. Line buffer: 121 bytes, flushed at 120 (wr.h:1580, 1595). ASan/UBSan runs clean, including my cases (buffer of size 1 and 3, buffer ending at the last word, `buf + size` wrapping 32 bits, 1499-character line).

### 7. No DMA possible: PASS
- EP Command (cfg 0x04) cannot be stored: no tuple names it (`swl_wr_ok(SWW_CFG, 0, 0x04, 0x6) < 0`, host test) and the glue refuses every config offset except 0x80 and 0x84 (glue:242). Bus master was observed off before stage 3 (`swl_pre_ep`: Command MEM-only is a precondition of every store).
- No BAR2 store after W14: W15-W17 contain no `swl_wr(…, SWW_BAR2, …)`; the older BAR2 tuples are consumed or exhausted after W14 (`SWT_ONCE` done bits, `img_next`, `tn[]`). The fake counts `bar2_stores_after_rel == 0` and `cfg04_writes == 0` in every case.
- No ring, doorbell or host_cap write: those would be BAR0 stores in the PCIe core window (the glue allows only IOCTL, RESET_CTL, bankidx, bound to their windows, glue:243-247) or BAR2 stores (none, above).
- See F3 (glue latch) and F4 (the log claim).

### 8. Worst case on the phone
- Hang: none unbounded; at most about 7 s more boot time.
- Panic: possible only as before this patch, if the link drops between the RC-local check and an MMIO read (asynchronous abort on the RC). The chance is somewhat higher now because running firmware can manage its own power state. A power cycle clears it.
- Damage: none found. All state is volatile chip RAM and wrapper registers; no flash, OTP, PMIC or host memory write; no DMA (s.7).
- Firmware misbehaviour with the link up and no host driver: it can only wait, sleep, trap or reset itself. Without bus master it cannot write host memory or raise MSI. Reads after a firmware reset return garbage or all-ones: printable filter, cached and re-checked bounds, read-only.
- Power cycle (Power + Volume Down, shim boot) fully reverts: the shim power-cycles the chip before the link comes up.

### 9. Tests: PASS with gaps (F2, F6)
- `rune()` covers: level 6 unchanged; full level 7 run; second look with ring wrap; 2 KiB cap; never up (100 polls, 4.95-5.1 s); pointer 0, all-ones, below RAM, unaligned, too close to the end, last place that fits, at ram_end; console pointer/buffer/size/index outside RAM (7 cases); second index outside; link lost while polling and before the second look; W15 state check; each release tuple gate alone (level, phase, order, companion, old); window tuple gates; `swl_bp_rd` bounds; store faults in W15; a buffer across the window boundary.
- All 25 new mutants are killed (run below).
- Important uncovered guards: the W15 restore arming (F2), a stop in W17 with the window on the tail (F1), the console line cap (F6).

## What I ran (host only)

| Check | Result |
|---|---|
| Header from the `shim/build.sh` chain (0129 new-file hunk + 0130, 0131, 0132, 0147, 0150 with `patch`) vs `final_header()` | byte-identical (`cmp`) |
| `gcc -O1 -Wall -Wextra -Werror` `shim/wlan_wr_host_test.c` against it | exit 0, `RESULT: PASS wlan_wr_host_test`, empty stderr |
| same with `-g -fsanitize=address,undefined -fno-sanitize-recover=undefined` | exit 0, `RESULT: PASS wlan_wr_host_test`, no sanitizer output |
| The 25 `run E` mutants of `tools/test_wlan_wr_mutants.py` (its own `build_and_run`, work dir in my scratchpad) | baseline pass; 25 of 25 KILLED |
| 20 mutants of my own | KILLED: X4, X5 (console buffer bounds), X6 (second-look link check), X7 (second index), X10 (cfg 0x84 restore arming), X12-X14 (poll period/count, settle time), X16 (no release), X17 (`RELEASED` report), X19 (console pointer bound). SURVIVED: X1 (F2), X2, X3, X8, X9, X11, X15, X18 (F6). X20 is the F1 fix shape (`swl_w84_zero` before `swl_done(SWF_W16)`): all checks still pass |
| Probe A (appended to a copy of the host test): shared 0x480000, console 0x470000, buffer 0x420000, link leaves L0 before the second look | `rc -8 row 17 armed_at_stop 0 restore none live cfg84 0x400000`; log `cfg84=0` (F1) |
| Probe B: same layout, one read refused in the W17 shared read, link alive | `rc -10 row 17 armed_at_stop 0 restore none live cfg84 0x400000`; log `cfg84=0` (F1) |
| Probe: store fault at each of the 5 stores of W15 | fault 1 (window select): nothing armed, `ca7=OUT OF RESET, CPUHALT`; faults 2-5: `restore ok`, CA7 `ctl 0x1`, `ca7=in reset` |
| Probe under ASan/UBSan: 1499-character console line; buffer size 1; size 3 ending 4 and 3 bytes before ram_end | no report; longest `fwcon:` payload 120; the buffer that reaches the last word is refused |

I did not run the full mutation suite (about 10 minutes, several hundred mutants); only the run E subset and my own.

## What I could not verify

- Anything on hardware: whether the firmware comes up with bus master off (the reference drivers run with it on: `pci_set_master` at `D/dhd_pcie_linux.c:1281`, 1348), what it prints, where the shared area and console lie (this decides whether F1 triggers), and how long the boot takes.
- Whether the CA7 has a second or third wrapper in the EROM. DHD would reset those too (`D/aiutils.c:1334-1344`); stage 1 records one wrapper per core. W6 on hardware halted the core through this one wrapper, so the release through it is the mirror image, but the EROM dump was not checked for more.
- Whether the link can leave L0 by the firmware's own action in the 5 s window, and whether a read on a link that drops between the check and the access aborts on this RC (inherited risk, not new code).
- What the released firmware does later while XNU keeps running without a driver (sleep, wake GPIO, PME). No handler is registered on the XNU side as far as this patch shows; I did not check interrupt masking of the PCIe1 RC.
- `delay()` behaviour and watchdog state at the point of boot where `sweet_wlan_init()` runs (the image builder passes `wdt=-1`; not checked on the kernel side).
- The kernel build of the patched tree (macOS CI) and that 0150 applies in the full patch series there; I only applied the header and glue chains on the host.
- Levels 5 and 6 on oriole have no run log yet (F8), so rows W11-W14 are unobserved on this phone.
