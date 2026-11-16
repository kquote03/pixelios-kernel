VERDICT: SAFE

# Review round 2 (final): xnu patch 0158 sweet-amp-init (`sweetamp=1`), independent safety review

Reviewer: Claude Fable 5.2 (delegate, worked alone, nothing run on the phone, patch not changed). Date 2026-11-16.
Commit reviewed: `39d80d7` on `wip/oriole-audio`, in a clean detached checkout (`/tmp/claude-1000/review-0158b`, removed afterwards). Delta since round 1: `git diff 808cf81..39d80d7 -- . ':!docs/reviews'` (commits `972e8be` tests, `39d80d7` fixes). Round 1: `docs/reviews/0158-amp-init-review-fable-1.md`.

Line refs:
- `h:N` = line N of `bsd/dev/sweet_amp.h` as extracted from the patch (= patch line N + 176).
- `c:N` = line N of `bsd/dev/sweet_amp.c` (= patch line N + 25).
- `test:N` = `shim/amp_xnu_host_test.c`; `mut:N` = `tools/amp_xnu_mutants.py`.
- AMP = `~/.hermes/cache/scratch/src-gm-amplifiers/cs35l41/` (git 33976d7). GKI = `ios-pixel6/pixel6-oriole-kernel/` (git a4662a84).
- XT = `~/pixelios-artifacts/localbuild/work/audio/xnu/`. K = `~/pixelios-artifacts/xnu-local-audio-39d80d7/kernel.development.sweet`, sha256 `54decba0cc312eb45d245e70a373f2cc7d99061273735719f993584e329d731a`.

Tags: **[obs]** = I ran it or read it in a built file or a recorded hardware log line; **[src]** = read from source at the cited line; **[guess]** = not shown by anything I could open.

## Summary

- The two fixes required before the phone run (F1, F2) are done, and F3, F4, F6, F8 are done as well. No finding of this round needs a change before the run.
- The delta opens no new hardware path. No writable MMIO word or bit was added, the amp write allowlist is still exactly the 12 stock B2 probe pairs, `PWR_CTRL1` (0x2014) is still unreachable, and the only new bus traffic is one more read of `IRQ1_STATUS1` per amp (a register and a frame the round-1 code already read).
- The new code only removes writes: an amp with `OTP_BOOT_ERR` or with an error bit before the init gets none, and R gets none after any failure on L.
- The A1 hardware value `IRQ1_STATUS3 = 0x20000000` passes the gate (bit 31 is the only bit checked, as in stock).
- `IRQ1_STATUS1` has never been read on this phone. I do not expect the new pre-write check to fail on a healthy amp, but it is not proven for bits 15 and 16. If it does fail, the run is fail-safe (zero writes) and the log does not show the value (N1).
- All findings of this round are low: a diagnostic gap, test gaps and stale texts. They should be fixed with the next regeneration of the patch, not before this run. Changing the patch now would only void this review.

## Findings

| id | sev | finding | required fix |
|---|---|---|---|
| N1 | L | The pre-write `IRQ1_STATUS1` value `st0` is read (`h:684`) but never printed. When an error bit is set before the init, the log has `!!! amp error bit in IRQ1_STATUS1 before the init: no write (H8)` (`h:686`) and `init writes=0 st1=00000000 ... FAILED` (`h:698-708`; `st1` is the post-write variable, still 0). Which of the seven bits was set is not in the log. This register has never been read on hardware, so a first-run stop here could not be diagnosed without a new patch and a new review. No hardware effect. | None before the run. With the next regeneration: print `st0` in the `init` line or in the `!!!` line. |
| N2 | L | Test gaps (the code is correct as read; these lines are pinned only by this review and by my scratch cases). Mutants that survive the committed test **[obs]**: the post-write error-bit check removed from `h:711` (n01) or its read removed from `h:692-693` (n20), because the fake amp's `st1` is constant and is now always caught by the pre-write check; the case `test:669-670` is labelled "after the init" but now stops before it. `AMP_SHORT_ERR`, `TEMP_ERR`, `TEMP_WARN`, `BST_SHORT_ERR` each dropped from `AMP_ERR_BITS` (n11, n17, n10, n18): round-1 F2 asked for each bit on each amp; only 0x80, 0x10000 and 0x40 have a case. The gate without `devid` (m15): round-1 F2 asked for a wrong-DEVID case; the fake amp has no DEVID knob. No case has `st3 = 0x20000000` (the hardware value). `tools/amp_xnu_mutants.py` is not called by `shim/build.sh` or `tools/check-all.sh`; it runs only by hand. | None before the run (this exact text was read and exercised: hostile cases X5b, X6d, N1, N2 below kill all of these). With the next regeneration: a fake amp that sets `st1` after write k, one case per error bit per amp, a wrong-DEVID case, a `st3 = 0x20000000` pass case, the script in the build. |
| N3 | L | Stale texts. The patch description still gives the pass line as `SWEET AMP: init ok l,r devid 35a40 rev b2, errors=0` (patch lines 7-8); the code prints `SWEET AMP: init ok (no otp unpack) l,r devid 35a40 rev b2, errors=0` (`h:999`, correct in the header comment `h:21-22`). The header comment lists six error bits and "After the writes" (`h:19-20`); the code checks seven bits, before and after (`h:188`, `h:684-688`, `h:709-711`). `test:2` still cites `cs35l41.c:4300-4302` (is `:4306-4308`). | None before the run. Correct with the next regeneration. |
| N4 | L | In a run where both amps identify but the init fails (error bit, read-back mismatch, restore ok), the line `SWEET AMP RESULT: l=0x35a40/0xb2 r=0x35a40/0xb2 restore=ok` is still printed (`h:997`); it is the A1 pass line. It is correct as an identify result, and `SWEET AMP: init FAILED` plus `SWEET AMP: verdict FAIL` follow. A reader or a tool that matches only the RESULT line would record a pass. | None in the patch. Record A7 from the `verdict` line (or `init ok (no otp unpack)`), never from the RESULT line. |
| N5 | L | The new pass line does not contain the plan's A7 line (`init ok l,r devid ...`, plan §11 A7) as a substring. That is the intent of F3, but a ladder check that greps the plan text will not match. | None in the patch. Record the step as "A7 partial (no OTP unpack)". |

Round-1 findings: F1 fixed, F2 fixed for the five named survivors (rest: N2), F3 fixed, F4 fixed, F6 fixed, F8 fixed (rest: N3). F5, F7, F9 are unchanged and now stated as known in the header comment (`h:24-27`); they needed no fix.

## Per-item verdicts

### 1. F1 fixed: PASS

- One gate macro, `AMP_ID_OK(ee, rr)` (`h:673`): `ee == AMP_OK` (every read of `amp_probe` worked, `h:730-752`), `rr.otp` (`OTP_BOOT_DONE` seen, `h:740`), `!(rr.st3 & AMP_OTP_BOOT_ERR)`, `rr.devid == AMP_DEVID` (0x35a40, `h:96`), `rr.revid == AMP_REVID` (0xb2, `h:97`) **[src]**.
- It gates the L writes (`h:981-982`) and, with `il == AMP_OK`, the R writes (`h:987-988`). `amp_init` is called from nowhere else, and `amp_write` only from `amp_init` (`h:689`) **[src]**.
- `OTP_BOOT_ERR` set: hostile cases X5c (L), X5c' (R), X5d (both), N1c (R `st3 = 0xA0000000`): zero write frames to that amp, no `init ok` line, rc = 1, exact restore **[obs]**. Committed case `test:674-675` **[obs]**.
- One value: `pass` (`h:998`) selects the line (`h:999`) and the return value (`h:1000`). `pass` needs `e`, `il`, `ir`, both `AMP_ID_OK` and `restored`. In 5,414 fault-injection runs (N5, N6) and every other hostile run, `rc == 0` exactly when the `init ok` line is present **[obs]**.
- The glue prints `SWEET AMP: verdict PASS (A7 partial: no otp unpack)` or `verdict FAIL (see the AMP lines)` from `amp_init_run() == 0` (`c:144`) **[src]**; both strings are in K **[obs]**.
- **Bit 29 of `IRQ1_STATUS3`** (A1 hardware: `st3=20000000` on both amps, pk-amp `device/google/oriole/docs/PORT.md:58` **[obs there]**):
  - It has no name in the pinned header. For this register AMP `cs35l41.h` names only `CS35L41_PLL_LOCK` 0x02 (`:692`), `CS35L41_PLL_UNLOCK` 0x10 (`:691`) and `CS35L41_OTP_BOOT_ERR` 0x80000000 (`:693`); `grep 0x20000000` over the whole AMP directory has no hit, and the GKI copy (`include/sound/cs35l41.h:689`) has no name for it either **[obs: grep]**. What the bit means is a **[guess]** (a boot or status flag; it reads the same on both chips).
  - Stock ignores it: the probe tests only bit 31 (AMP `cs35l41.c:4244-4249`), the interrupt handler acts only on `PLL_LOCK` in this register (`:2040-2042`), and `CS35L41_INT3_MASK_DEFAULT` 0xFFFF87FF (`cs35l41.h:706`) keeps bit 29 masked **[src]**. Both amps probe on stock (`~/pixelios-artifacts/ref/oriole/20261114T072643Z/dmesg.txt:1641`, `:1813`) **[obs]**.
  - The gate tests only `AMP_OTP_BOOT_ERR` = 0x80000000 (`h:99`, `h:673`), so it treats bit 29 exactly as stock does. Hostile N1: `st3 = 0x20000000` on both amps gives 12 + 12 writes and the pass line; N1b: every bit but 31 set passes **[obs]**. Correct.

### 2. F2 fixed: PASS (rest in N2)

- My five round-1 survivors against the committed test at `39d80d7` **[obs]**: m08 (`BST_DCM_UVP_ERR` dropped) killed by `test:681-682`; m09 (no other-CS check in `amp_write`) killed by `test:688-689` (the fake bus reports "both chip selects low"); m10 (gate without `otp`) killed by `test:677-678`; m12 (read-backs not compared) killed by `test:686-687`; m13 (gate without `revid`) killed by `test:679-680`.
- `tools/amp_xnu_mutants.py` **[obs]**: `RESULT: PASS 11/11 mutants killed`, exit 0. Checked separately: its header extraction is byte-identical to mine; each of the 11 patterns occurs exactly once (the script stops otherwise, `mut:59-61`); all 11 mutants compile with the script's flags, so each is killed by a failing `CHECK`, not by a compile error.
- The equivalent mutant (`mut:22`: removing the by-name `PWR_CTRL1` refusal `h:200-201` alone) is really equivalent: `amp_write_ok` returns 1 only for a pair of `AMP_INIT_SEQ` (`h:202-203`) and no pair names 0x2014 (`h:191-193`) **[src]**. My m14 (that removal) survives the committed test and my hostile test, including `amp_write_ok(0x2014, v)` and `(0x80002014, v)` for all 65,536 low values **[obs]**. The "13th pair" mutant shows the refusal works when it matters.
- Not complete: N2.

### 3. F3: PASS

- Pass line: `SWEET AMP: init ok (no otp unpack) l,r devid 35a40 rev b2, errors=0` (`h:999`); in K once, and the old line without `(no otp unpack)` is not in K **[obs]**.
- Header: "Any later step that sets GLOBAL_EN must first add the OTP unpack" (`h:22`) **[src]**. Glue verdict: "PASS (A7 partial: no otp unpack)" (`c:144`).
- The patch description still quotes the old line (N3).

### 4. F4 and F6: PASS, no new path

- F4: `if (il == AMP_OK && AMP_ID_OK(er, r)) ir = amp_init(...)` (`h:987-988`). R is still released and identified (reads only) after a failed L. Hostile: L not B2, L DEVID wrong, L no `OTP_BOOT_DONE`, L `OTP_BOOT_ERR`, L error bit before or after its writes, L read-back mismatch (N8): R gets 0 writes in each; over all 5,400+ fault runs R never has a write unless L has exactly 12 **[obs]**.
- F6: `AMP_ERR_BITS` = 0x800381C0 (`h:188`: bits 31, 17, 16, 15, 8, 7, 6). `amp_init` reads `IRQ1_STATUS1` first (`h:684`); a failed read or any error bit sets `e`, and every write is behind `e == AMP_OK` (`h:689`) **[src]**. Hostile X6d: each of the 7 bits on each amp before the init: zero writes to that amp, 14/14; N4: a bit that is set at the pre-write read and gone afterwards: zero writes; N2: each bit appearing only after write 1 or write 12: 12 writes, `!!! amp error bit in IRQ1_STATUS1 (H8 ...`, no pass, 28/28 **[obs]**.
- No new path: the pre-write read is `amp_read` of 0x10010, the same call the round-1 code made after the writes (`h:693`); `tx_byte_ok` (`h:389-394`), `w32_allowed`, `amp_reg_writable` are unchanged by the delta **[obs: diff of the two extracted headers, changed lines 6, 9, 16, 21-27, 188, 195, 672-674, 678, 684-688, 920, 981, 987, 998-1000 only]**. Reading the register does not clear it: stock clears a bit by writing it (AMP `cs35l41.c:2060-2061`, `:2345`) **[src]**.
- **Could a normally-set bit make the run fail every time?** Not likely, not proven.
  - Hardware: the A1 run read `IRQ1_STATUS4` (0x3), `IRQ1_STATUS3` (0x20000000), DEVID and REVID only. `IRQ1_STATUS1` has never been read on this phone **[obs: PORT.md:58]**. So there is no A1 value to compare.
  - Stock never reads or clears `IRQ1_STATUS1` in the probe; its first read is in the interrupt handler (AMP `cs35l41.c:2021-2024`) **[src]**. Stock unmasks bits 31, 17, 16, 8, 7, 6 at probe (`:4307-4308`), and the handler logs `dev_crit` for 31, 17, 8, 7, 6 (and 15) (`:2058-2141`). The stock dmesg of this phone has no such line after the two probe lines **[obs: `grep -i "cs35l41\|Amp short\|Over temperature\|VBST\|LBST"`, dmesg.txt]**. So bits 31, 17, 8, 7, 6 were not set after a reset release on stock (assuming the interrupt was delivered: **[guess]**).
  - Bit 16: unmasked by stock, no handler branch, and the handler never clears it (`:2008-2165`) **[src]**. If it were set after every reset, the amp's interrupt would stay asserted on every stock boot. That it is normally 0 is an inference: **[guess]**.
  - Bit 15 (`TEMP_WARN`): masked in stock, so stock gives no evidence. An over-temperature warning on an amp that has never been enabled is not expected: **[guess]**.
  - Bits outside `AMP_ERR_BITS` do not block: N3, `st1 = 0x7FFC7E3F` on both amps passes **[obs]**.
  - If the check does fire, the run is safe (no write to that amp, none to R after L, exact restore) but tells only that one of seven bits was set (N1).

### 5. No new writable word, bit or amp pair: PASS

- `amp_reg_writable` (`h:397-402`, 21 words), `w32_allowed` (`h:420-482`), `w32` (`h:486-499`), `AMP_INIT_SEQ` (`h:191-193`), `amp_write_ok` (`h:196-206`), `wframe_byte` (`h:209-213`) are not in the delta **[obs: diff]**.
- Sweep re-run **[obs]**: `amp_write_ok` over 76,230 pairs (10 registers incl. 0x2014 x 7 values x every single-bit flip of register and value): 12 allowed, 0 differences from an independently typed 12-pair table.
- `PWR_CTRL1` **[obs]**: `amp_write` of (0x2014, 1), (0x2014, 0), (0x80002014, 1), `PWR_CTRL2` 0x2018: error, 0 MMIO writes, 0 frames (X2b-X2e). A stale arm of (0x2014, 1) before the run: 0 frames, run fails, exact restore (X2). X10: over the unarmed state and every armed pair (the 12 and 0x2014), no position-3 byte 0x14 is accepted, so address 0x00002014 cannot be spelled; 0x2018 can be spelled only by mixing arm states (F5, not a code path).
- The fake amp counts every write frame against the 12-pair order (`test:225-231`); no out-of-sequence write in any committed or hostile run **[obs]**.

### 6. Generated header = pk-amp `ampid.c` at 4c7be85 + the A7 additions: PASS

- `diff -w` of the header from `#define AMP_BASE` on against `git -C pk-amp show 4c7be85:shim/ampid.c` **[obs]**: the only differences are the three read registers in `tx_byte_ok`, the arm arguments and armed branches of `w32_allowed` / `w32`, the `tx == 2` case of `xfer`, `amp_write`, `AMP_ID_OK`, `amp_init`, one text ("no MISO answer"), and `amp_init_run` (name, `il` / `ir` / `pass`, the two gated `amp_init` calls, the pass line and return). `pads_why` (`h:810-833`, any reset DAT bit 1 refuses `h:829-831`), `restore` (`h:838-882`, read-modify-write of the reset and chip-select bits `h:844-845`, `h:850-851`, amp fields only and resets must read 0 `h:878-881`) and `put_amp` (`h:777-786`, `none` unless read, OTP done, no OTP error `h:780`) are identical to 4c7be85.
- The constants (`h:47-149`, 103 `#define`s): each equals a line of `tools/boards.py oriole --amp-board` or of the generated `board_config.h` in pk-amp (generator, `soc.json`, `board.json` unchanged since 4c7be85) **[obs]**.
- pk-amp `shim/` is unchanged after 4c7be85 (`git log 4c7be85..HEAD -- shim/` empty); that commit is the one reviewed SAFE (`bad1d07`) and the one of the hardware PASS `o5-a1-ampid` **[obs: PORT.md:58]**.

### 7. No regression of round 1: PASS

- Frame discipline, chip selects, arming: not in the delta. Re-run **[obs]**: X1 (stale arm), X4 (9 / 11 bytes, `PACKET_CNT` 9 / 11, armed TX-on at level 6, unarmed TX-on at level 10, armed 0x80 at position 0, a second frame behind a full one): all refused, 0 frames. X8: write to R with CS0 low, write to L with CS1 low, read of L with CS1 low: 0 MMIO writes, 0 frames. The arm is clear after every run. No "both chip selects low", no write to an amp in reset in any run.
- Bounded waits: one more read per amp (2 transfers, at most 40 ms). Per amp 4 + 100 x 41 + 4 x 40 + 40 + 12 x 20 + 3 x 40 = 4,664 ms; both about 9.3 s, as the glue and header now state (`c:7`, `h:22`). No new loop **[src]**.
- Gate: `sweetamp` must be 1 (`c:121-127`), root `model-number` exactly "oriole" with its NUL (`c:128-133`), one run per boot (`c:117-120`); unchanged except the comment and `c:144` **[obs: diff]**.
- Ordering: `sweet_amp_init()` after `sweet_keys_gs101_start()`, before `sweet_touch_gs101_init()`, `sweet_gpu_gs101_init()`, `sweet_wlan_init()`, `sweet_batt_init()` (patch lines 1192-1198; XT `bsd/dev/sweet_ufs.c:708`) **[src]**; the `sweet_ufs.c` and `conf/files` hunks are identical to round 1 **[obs]**.
- Restore: no `return` between the first write (`h:957`) and `restore()` (`h:992`); it drives both resets low first (`h:844-845`) and requires both to read 0 (`h:881`). In every hostile run `restore=ok` appears only with an exact restore; N7: both inits good and the restore dropped gives `!!! RESTORE FAILED`, no pass line **[obs]**. Round-1 mutants m17 and n15 (restore not driving RST_L / RST_R low) are killed **[obs]**.
- The patched tree matches the patch: `cmp` of the two extracted files against XT `bsd/dev/sweet_amp.h` and `sweet_amp.c`: identical **[obs]**.

### 8. Anything new

- N1-N5 above. Nothing that affects the hardware.
- The amp's interrupt pin is not routed by this patch (stock does that in `cs35l41_irq_gpio_config`, AMP `cs35l41.c:4276`, which is not ported), so unmasking `IRQ1_MASK1` (round-1 F9) most likely drives no pad at all **[src for the missing call; guess for the chip default]**.
- With an error bit found before the init, the bit is not cleared (no write) and the amp goes back into reset at the restore. Correct for H8.

## Hostile cases (scratch copy of the committed test with a DEVID knob, a late-status knob and a clear-on-read knob added to the fake amp; the header from the patch; ASan + UBSan; not committed)

Every full run also checks: no fake-bus violation, `rc == 0` exactly when `init ok` is printed, never both `init ok` and `init FAILED`, `restore=ok` only with an exact restore, no out-of-sequence write, arm clear afterwards, R written only if L got all 12.

| # | scenario | result **[obs]** |
|---|---|---|
| X1 | arm left on for an allowed pair before the run | rc 1, 0 frames, 0 amp writes, exact restore |
| X2 | armed with (0x2014, 1) before the run; `amp_write` of 0x2014 (1 and 0), 0x80002014, 0x2018 | rc 1, 0 frames; each `amp_write` an error with 0 MMIO writes |
| X3 | value one bit off, address one bit off, 0x381C = 0x2051, TEST_KEY = 0x55 / 0xAA; sweep | all refused, 0 MMIO writes; 76,230 pairs, 12 allowed, 0 differ |
| X4 | frame lengths and arm-state mismatches at the `w32` level | all refused, 0 frames |
| X5a / b / e | L REVID 0xB0; R DEVID 0x35B40; L DEVID 0x35B40; R REVID 0xB1; L or R never `OTP_BOOT_DONE` | 0 writes to that amp; rc 1; R also 0 when L is the bad one; L 12 when R is the bad one |
| X5c / c' / d | `OTP_BOOT_ERR` on L / on R / on both | 0 writes to each such amp, no `init ok`, rc 1 (round 1: 12 writes and the pass line) |
| X6a / b | `BST_SHORT_ERR` on L; `AMP_SHORT_ERR` on both | 0 writes to L and R, "before the init" line |
| X6c | bit 16 on L | rc 1, 0 writes (round 1: pass line) |
| X6d | each of the 7 bits on each amp, set from the start | 14/14: rc 1, 0 writes to that amp, exact restore |
| X8 | write to R with CS0 low; write / read of L with CS1 low | 0 MMIO writes, 0 frames |
| X9 | `w32`-level mix: armed header bytes, disarm, unarmed TX-on | still accepted by the allowlist (F5, known, not a code path) |
| X10 | union of accepted address bytes over all arm states | 0x2014 not composable (byte 3 = 0x14 never accepted); 0x2018 composable |
| N1 | `st3 = 0x20000000` on both (the hardware value); every `st3` bit but 31; R `st3 = 0xA0000000` | pass with 12 + 12 writes; pass; rc 1 with L 12, R 0 |
| N2 | each of the 7 bits appearing only after write 1 or write 12, each amp | 28/28: rc 1, post-write `!!!` line, no R write after L |
| N3 | `st1 = 0x7FFC7E3F` (every non-error bit) on both | pass, 12 + 12 writes |
| N4 | an error bit set at the pre-write read and clear afterwards (L; R) | rc 1, 0 writes to that amp |
| N5 | read faults: STATUS / CH_CFG / RX_DATA / gpp25 DAT fail from read k on, 3,304 runs | 0 runs break an invariant |
| N6 | a faulting write and dropped writes at every write position, 2,110 runs | 0 runs break an invariant; never a pass |
| N7 | both inits good, the restore writes dropped | rc 1, no `init ok`, `!!! RESTORE FAILED` |
| N8 | L `IRQ1_MASK1` read-back mismatch | rc 1, L 12 writes, R 0 |
| N9 | a failed run, then a good run in the same process | the second passes (no state left) |

Total: 66,418 checks, 0 failed; the 41 `ok:` lines of the committed cases first.

## Mutants (header from the patch, each compiled against the committed test with the build's flags, and against my hostile test)

The round-1 scratch files were deleted with that checkout; m01-m20 are rebuilt from the round-1 descriptions.

| mutant | committed test | hostile |
|---|---|---|
| m01 `PWR_CTRL1` added to the sequence; m03 wrong value; m04 swapped order; m05 L written without identification; m06 arm never cleared; m07 armed byte not checked against `amp_write_ok`; m11 unarmed TX-on with 10 bytes; m16 little-endian value; m17 restore not driving RST_L low; m18 pass line always printed; m19 error bits never checked; m20 gate without `OTP_BOOT_ERR` | killed | killed |
| m08, m09, m10, m12, m13 (the five round-1 survivors) | **killed** (m09 by a compile error under `-Werror`, and by a failing case under the script's `-w`) | killed |
| m02 `amp_write` without its own `amp_write_ok` check | survives (no frame reaches an amp: the armed TX check refuses) | killed (the chip select is written before the refusal) |
| m14 by-name `PWR_CTRL1` refusal removed | survives | survives: equivalent |
| m15 gate without `devid` | **survives** (N2) | killed |
| n01 post-write error check removed; n20 post-write `IRQ1_STATUS1` read removed | **survive** (N2) | killed |
| n10 `TEMP_WARN`, n11 `AMP_SHORT_ERR`, n17 `TEMP_ERR`, n18 `BST_SHORT_ERR` dropped | **survive** (N2) | killed |
| n02 pre-write check removed; n03 pre-write read result ignored; n04 R written after a failed L; n05 pass without `restored`; n07 pass without `ir`; n08 return always 0; n09 bit 16 dropped; n13 R gate without ID; n15 restore not driving RST_R low; n16 armed TX-on without `amp_write_ok`; n19 `BST_OVP_ERR` dropped | killed | killed |
| n06 pass without `il`; n14 pass without `AMP_ID_OK` | survive | survive: redundant (`ir` is set only when `il` is ok; `il` / `ir` are set only behind `AMP_ID_OK`) |

## Commands run and results

All through `pixos-run.sh`. All **[obs]**.

1. Clean checkout: `git -C pk-audio worktree add --detach /tmp/claude-1000/review-0158b 39d80d7`; removed at the end.
2. `cd shim && PIXELIOS_HOST_TESTS=always ./build.sh --board oriole --avb --pstore-fast --usb-gadget --out out/rev0158b` under `flock ~/pixelios-artifacts/scratch/heavy.lock` → exit 0, 308 lines starting with `PASS`, no line starting with `FAIL`. The header the build extracted (`out/rev0158b/sweet_amp.h`) is byte-identical to the one I reviewed. Its amp lines:
   - `PASS amp_xnu_host_test (compile)`
   - `ok: allowlist unit cases`
   - `ok: armed write unit cases`
   - `ok: full run, mux on the TOP source (switched to oscclk and back) (1055 writes, 2478 reads, 44 frames)`
   - `ok: full run, mux already on oscclk (never written) (1053 writes, 2468 reads, 44 frames)`
   - `ok: stopped and restored: L OTP_BOOT_ERR (A7 gate, review F1) (351 writes)`
   - `ok: stopped and restored: L never OTP_BOOT_DONE (gate needs l.otp) (2776 writes)`
   - `ok: stopped and restored: R is revision B0 (gate needs r.revid) (703 writes)`
   - `ok: stopped and restored: L BST_DCM_UVP_ERR (error bits include 0x80) (376 writes)`
   - `ok: stopped and restored: L bit 16 (review F6) (376 writes)`
   - `ok: stopped and restored: L IRQ1_MASK1 write does not stick (read-backs compared) (703 writes)`
   - `ok: stopped and restored: CS1 stuck low before the L writes (amp_write checks the other CS) (376 writes)`
   - `ok: a faulting write at each write position of a full run stops, restores, never claims a false ok`
   - `ok: dropped writes from any position never pass`
   - `ok: a dropped restore write is loud (!!! RESTORE FAILED, restore=FAILED)`
   - `PASS amp_xnu_host_test` (41 `ok:` lines; a full run is 44 frames now, 42 in round 1: one more read per amp)
   - `PASS amp_xnu_asan_host_test (compile)`
   - `PASS amp_xnu_asan_host_test`
3. `python3 -B tools/amp_xnu_mutants.py` → 11 lines `killed: ...`, `RESULT: PASS 11/11 mutants killed`, exit 0.
4. `tools/patch-numbers check` → `RESULT: PASS this tree's patch numbers do not overlap other branches or reservations`, exit 0.
5. `grep -a -o` of the AMP strings in K →
   - `SWEET AMP: refused sweetamp=%u (only 1 is defined), not touched`
   - `SWEET AMP: refused not an oriole ADT (model-number), not touched`
   - `SWEET AMP: refused ml_io_map(%#llx) failed, not touched`
   - `SWEET AMP: init run (sweetamp=1), timebase %llu Hz`
   - `SWEET AMP: verdict %s`, `PASS (A7 partial: no otp unpack)`, `FAIL (see the AMP lines)`
   - `AMP: init ok (no otp unpack) l,r devid 35a40 rev b2, errors=0` (once), `AMP: init FAILED (see the AMP lines)`
   - ` !!! amp error bit in IRQ1_STATUS1 before the init: no write (H8)`, ` !!! amp error bit in IRQ1_STATUS1 (H8: no output steps for the rest of the run)`, `!!! RESTORE FAILED: `, `sweetamp`
6. `cmp` of the patch's `bsd/dev/sweet_amp.h` / `sweet_amp.c` against XT → identical. XT `bsd/dev/sweet_ufs.c:708` has the call; XT `bsd/conf/files:122` has the file.
7. `diff -w` of the header tail against pk-amp `4c7be85:shim/ampid.c`; the 103 constants against the pk-amp generator output → item 6.
8. `diff` of the round-1 and round-2 extracted headers and glue → the changed lines listed in item 4 and item 7.
9. Hostile test (`-fsanitize=address,undefined -fno-sanitize-recover=all`) → exit 0, no sanitizer report, table above.
10. 39 reviewer mutants + the 11 of the script, each checked for "pattern occurs once" and "compiles" → table above.
11. `grep` of the stock dmesg and of AMP / GKI headers for the status bits → items 1 and 4.

Not run: any phone command; a kernel build.

## What I could not verify

- Anything on hardware. In particular the value of `IRQ1_STATUS1` on a freshly released amp of this phone (item 4), and whether the chip returns `IRQ1_MASK1` as written.
- The meaning of `IRQ1_STATUS3` bit 29 and of `IRQ1_STATUS1` bit 16 (no datasheet in the pinned sources).
- The compiled kernel beyond its strings and its date: no disassembly, no object audit. That K was built from XT as it is now is supported by the strings (the new pass line, the new `before the init` line, the verdict strings; the old pass line absent) and by the file times, not proven.
- The round-1 open points that need the datasheet (F7) or tracing of ADT-driven drivers (round-1 item 7); unchanged by this delta.
- Whether a Fable review of Opus-written code satisfies AGENTS rule 6.
