VERDICT: SAFE

# Delta review: fixes to xnu patch 0150 sweet-wlan-fw-ready (sweetwlan=7), independent, different model family

Reviewer: Claude Fable 5.2 (delegate, worked alone, read-only, no phone access, no commit). Author family: Claude Opus. Date 2026-11-15.
Range `4506d56..ef93d09` on `wip/oriole-next`. `~/.hermes/cache/scratch/review/0150-d2.diff` is byte-identical to `git diff 4506d56..ef93d09 -- shim tools xnu` (`cmp`).
Line refs: `wr.h:N` = `bsd/dev/sweet_wlan_wr.h` from `final_header()` at `ef93d09`; `0150:N` = line of the patch file; `test:N` = `shim/wlan_wr_host_test.c`; `mut:N` = `tools/test_wlan_wr_mutants.py`.
Review 1: `docs/reviews/0150-wlan-fw-ready-review-fable-1.md` (SAFE-AFTER-FIXES, F1 and F2 required).

Summary: F1 and F2 are fixed and pinned by tests. The header changes by one statement (`swl_w84_zero` at the end of W16), one report expression (`swl_left`), one log string and four comments; the glue is byte-identical. All remaining findings are severity L and none blocks one `sweetwlan=7` run. F3 stays required before the next level adds a store.

## Findings

| id | sev | finding | required fix |
|---|---|---|---|
| G1 | L | F3 of review 1 is still open: the glue has no independent gate for the CA7 release and no latch against BAR2 stores after it (glue unchanged in this delta). For this run it is acceptable: run E has no BAR2 store and no tuple that could issue one (review 1 s.7; fake counts `bar2_stores_after_rel == 0` in every case of my probe). | None for the `sweetwlan=7` run. Required before any level that adds a host_cap, ring or other store after the release. |
| G2 | L | F5 is fixed only in part. Fixed: wr.h:1460 (2 s DHD timeout, bwfm 869-875), wr.h:1467 (`bcmpcie.h:45-49`), wr.h:1496 (`if_bwfm_pci.c:880-881`); I re-opened the three sources and they match. Still wrong: patch header 0150:5 ("bwfm if_bwfm_pci.c:868-874 ... DHD 5 s") and the W16 function comment wr.h:1531-1532 ("5 s", "868-881"). F5(d) (cite `bcmpcie.h:384` for `rings_info_ptr`, wr.h:1465) and F5(e) (plan text "<= 5 s") were not done. | Fix the two comment texts in the next edit of the patch. No code change. |
| G3 | L | F6 is fixed for the line cap only (new case test:2501-2514, mutant mut:643 killed; without the cap the plain build dies with SIGSEGV and ASan reports `stack-buffer-overflow` at wr.h:1605). The other survivors of review 1 (X2, X3, X8, X9, X11, X15) are neither listed EQUIVALENT nor covered. | List them EQUIVALENT with the implying clause, or add cases. Not needed for the run. |
| G4 | L | The new `swl_left` branch `TAIL WINDOW (run E, restore not armed)` (wr.h:761) cannot be reached any more (0 of 5334 stops in my probe) and no test pins it: putting the old expression back survives (my mutant Y5). It is a defensive report line; harmless. | Optional: list as EQUIVALENT (implied by `swl_w84_zero` before every `swl_done` of W16/W17). |
| G5 | L | The F2 mutant removes `.rgrp` from all four `SWT_CAR_*` tuples at once. Removing it from `SWT_CAR_ASSERT` alone is killed; from `IOCTL_MID`, `RELEASE` or `IOCTL_END` alone survives (my mutants Y1-Y4). This is equivalent, not a hole: `x->armed` is sticky from the first store attempt (wr.h:856) until `swl_done`. | None. Optionally note it at mut:641. |
| G6 | L | The F1 host case (test:2495-2499) pins the end state of W16 only. The W17 stops I asked for (read refused, link lost, with everything above 0x400000) are not in the suite. My probe covers them (below) and the mutant is killed through the W16 check. | Optional: add the two W17 stop cases. |

## Requirements

### 1. F1 fixed: PASS
- W16 ends on window 0 on success: `SWW_TRY(swl_w84_zero(x))` at wr.h:1573, before `swl_done(x, SWF_W16)` at wr.h:1574 (0150:311). If that store fails, `swl_wr` stops while `SWW_RG_W84` is still armed (armed since the tail move, wr.h:856; cleared only after a good store, wr.h:1525), so the restore is attempted.
- No path after W16 leaves cfg 0x84 at 0x400000 unarmed:
  - W17 starts with `x->w84 == 0`, `x->armed == 0` (test:2499 asserts `f.win2 == 0 && x.w84 == 0 && x.armed == 0`).
  - Every move to the tail in W17 goes through `swl_bp_rd` -> `swl_wr` (wr.h:1508-1509), which arms the group before the store (wr.h:856).
  - Every `swl_done(x, SWF_W17)` is preceded by `swl_w84_zero` (wr.h:1638-1639, 1647-1648, 1669-1671).
  - The only `swl_stop` in W17 outside `swl_bp_rd`/`swl_wr` is the link-lost stop (wr.h:1665-1667); the group is armed there if the window is on the tail, and the restore is reported as skipped.
- `swl_left` is truthful: wr.h:761 reports `0`, `0 (restored)`, `TAIL WINDOW (restore not done)` or, if the group was not armed but `x->w84 != 0` in run E, `TAIL WINDOW (run E, restore not armed)`.
- Probe (mine, under ASan/UBSan, 4 layouts of shared/console/buffer incl. all above 0x400000, all below, straddling): every read of run E refused, the restore store then failing or dropped, every store of run E failing / dropped / corrupted / followed by link loss, link loss every 12.5 ms for 8 s after the release, firmware never up. Result: `stops 5334, stops with live cfg84 != 0: 2280 (reported TAIL: 2280), 'not armed' lines 0, violations 0`. Every stop with the live window on the tail had the restore group armed; every success ended with cfg 0x84 = 0.
- Probe A of review 1 (link lost before the second look, everything above 0x400000) now gives `armed_at_stop 0x8 restore skipped`, `cfg84=TAIL WINDOW (restore not done)` (was `restore none`, `cfg84=0`). Probe B (read refused in W17) gives `restore ok`, `cfg84=0 (restored)`, live 0.

### 2. F2 fixed: PASS
- test:2491-2492: for a fault at the window select (k = 1) `x.restore == SWW_RS_NONE && f.ca.ctl == 0`; for faults at the four release stores (k = 2..5) `x.restore == SWW_RS_OK && (f.ca.ctl & 1u) && (x.rs_ok & SWW_RG_CA)` and the text `ca7=in reset`.
- Mutant `run E W15 restore: no CA7 reset restore group` (mut:641, pattern matches the 4 tuples): KILLED, 4 failed checks. See G5.

### 3. F4-F7 handled, F3 and F8 open: PASS with G1-G3
- F4: the log line no longer claims an observation: `cfg84 back at 0 (no EP Command write exists in run E)` (wr.h:1670). True as read from source (review 1 s.7).
- F5: partly (G2).
- F6: line-cap case and mutant added; rest open (G3).
- F7: stated in the patch header (0150:13): a stop in W16/W17 leaves the CA7 released and the firmware running.
- F3 open: acceptable for one `sweetwlan=7` run (G1).
- F8: closed by evidence, not open. *Observed in logs* (not by me on hardware): `logs/runs/20261114T194515Z-o2-w2-l5` (`sweetwlan=5`, `stage 3 level 5 done vars=21388 token=0xeb1c14e3 crc=0x23bf8f85`), `20261114T195113Z-o2-w3-l6` and `20261114T200516Z-o2-w3-l6ci` (`sweetwlan=6`, `stage 3 level 6 done w14 old=0xba3ef000 new=0xbbbef214`), no `STOP` line in any of the three, `ram_end 0x4a0000` (the layout the host test uses). Rows W8-W14 have therefore run on this phone. I took "oriole" from the task statement and the `o2` label; I did not find a board field to confirm it.

### 4. No new store, tuple or unbounded loop; nothing changes at sweetwlan<=6: PASS
- Header before vs after the delta (old 0150 vs new 0150 on the same chain, `diff`): one comment and the `w84` expression in `swl_left` (wr.h:758, 761), three citation comments (wr.h:1460, 1467, 1496), the new statement wr.h:1573, the log string wr.h:1670. Nothing else. Glue: identical. `SWT_N` and `swl_wt[]` unchanged (34 tuples), `swl_rs[]` unchanged.
- No new store statement and no new tuple. The fix adds one call of the existing `swl_w84_zero` (existing tuple `SWT_W84_ZERO_E`, cfg 0x84 := 0, through `swl_wr`). Effect on a good run: up to two more cfg 0x84 stores (back to 0 at the end of W16, to the tail again in W17 when the shared area is above 0x400000); run E in the default layout goes from 9 to 11 stores, all cfg 0x80/0x84 or the four release stores.
- No loop added or changed.
- Level <= 6: wr.h:1573 is inside `swl_w16`, reached only at level 7 (wr.h:1729 gate). The `swl_left` change needs `x->flags & SWF_W15`, which is never set below level 7, so the line prints `0` as before. The level 3-6 host cases pass unchanged.

### 5. Worst case on the phone: unchanged from review 1
- Hang: none unbounded; about 7 s more boot time.
- Panic: possible as before if the link drops between the RC-local check and an MMIO read. A power cycle clears it.
- Damage: none found. Volatile chip RAM and wrapper registers only; no flash, OTP, PMIC or host memory write; no DMA (EP Command cannot be stored).
- Power cycle (Power + Volume Down) fully reverts: yes. The delta adds only cfg 0x84 stores of the two values already used.

## What I ran (host only, through `.pixos-fhs/bin/pixos-fhs`)

| Check | Result |
|---|---|
| `gcc -O1 -Wall -Wextra -Werror` `shim/wlan_wr_host_test.c` against `final_header()` | compile rc 0, run rc 0, `RESULT: PASS wlan_wr_host_test` |
| same with `-g -fsanitize=address,undefined -fno-sanitize-recover=undefined` | compile rc 0, run rc 0, `RESULT: PASS wlan_wr_host_test`, no sanitizer output (stderr is that one line, 31 bytes) |
| Harness baseline (`build_and_run`, both host tests) | pass |
| Mutant `run E W15 restore: no CA7 reset restore group` | KILLED: `RESULT: FAIL wlan_wr_host_test (4 checks)` at the test:2492 check |
| Mutant `run E W16: ends on the tail window` | KILLED: `FAIL line 2499: swl_w15(&x) == 0 && swl_w16(&x) == 0 && f.win2 == 0 && x.w84 == 0 && x.armed == 0 && x.shared == 0x480000u`, 1 check |
| Mutant `run E con_out: no line cap` | KILLED: plain build exits with signal 11; ASan build: `index 121 out of bounds for type 'char [121]'`, `stack-buffer-overflow` (wr.h:1605) |
| My mutants | Y1 `.rgrp` off `SWT_CAR_ASSERT` only: KILLED. Y2-Y4 off one of the other three: SURVIVED (G5, equivalent). Y5 old `swl_left` expression: SURVIVED (G4) |
| My probe (copy of the host test with its own `main`, ASan/UBSan) | `stops 5334, stops with live cfg84 != 0: 2280 (reported TAIL: 2280), 'not armed' lines 0, violations 0`, exit 0 |
| Header and glue before/after the delta | header: the lines listed in s.4; glue identical |

Not run: the full mutation suite (375 mutants) and `tools/check-all.sh`. Only the three named mutants, the harness baseline and my own.

## What I could not verify

- Anything on hardware: whether the firmware comes up with bus master off, where the shared area and console lie, what it prints, boot time.
- That the three level 5/6 run logs are from oriole (label and task statement only), and that the kernel for the level 7 run contains this exact patch text (kernel build on the macOS CI or `tools/xnu-local-build` not run by me).
- That 0150 applies in the full patch series; I applied only the header and glue chains on the host.
- The open items of review 1 that need hardware or other code: more CA7 wrappers in the EROM, link behaviour once the firmware runs, what the firmware does later without a host driver, `delay()` and watchdog state at that point of boot.
