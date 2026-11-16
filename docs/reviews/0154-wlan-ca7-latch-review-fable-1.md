VERDICT: SAFE

# Review: xnu patch 0154 sweet-wlan-ca7-latch (release gate, BAR2 latch, board-bound firmware identity)

Reviewer: Claude Fable 5.2 (independent, worked alone, did not write the patch, no phone access, nothing run on the phone, patch not changed). Date 2026-11-16.
Range `7cfcbf7..a370142` on `wip/oriole-w7`; `~/pixelios-artifacts/scratch/review/0154-w7.diff` is byte-identical to `git diff 7cfcbf7..a370142` (`cmp`, observed).
Line refs: `glue:N` = `bsd/dev/sweet_wlan.c`, `wr.h:N` = `bsd/dev/sweet_wlan_wr.h`, both in `~/pixelios-artifacts/localbuild/work/w7/xnu/`; `test:N` = `shim/wlan_wr_host_test.c`; `mut:N` = `tools/test_wlan_wr_mutants.py` (both at `a370142`).
The patched `wr.h` there is byte-identical to the patch chain (`final_header()`); the patched glue differs from the mutant tool's chain only by patch 0149's read-only probe, so `sw3_ptr`/`sw3_wr` are the same bytes (observed, see N1).

Summary: the patch only adds refusals. With the real `sw3_wr()` in the loop, levels 3-7 on both boards issue the same stores and print the same log before and after the patch (observed, my probe). The gate, the latch and the board binding do what the patch says. Nothing in the range can write EP Command, touch S2MPU/SysMMU, or reach a new register. All findings are severity L; none blocks a run at levels <= 7. F1 and F2 should be closed in the patch that adds the first store after the release, because that patch will rely on the latch.

## Findings

| id | sev | finding | required fix |
|----|-----|---------|--------------|
| F1 | L | The glue gate covers one of the two ways to start the CA7. It refuses an IOCTL value without CPUHALT (glue:255-257), but `sw3_wr()` accepts `RESET_CTL := 0` on the CA7 wrapper at any level without looking at the live IOCTL (glue:250, glue:262; observed: my probe P1, levels 3-6, IOCTL shadow 0, returns 0, `released` stays 0). The wrapper powers up as IOCTL 0 / RESET_CTL 1 (test:2-3, read from source), so "out of reset with CPUHALT clear" below level 7 is still prevented by the header alone (`SWT_CA_RELEASE` `.cval = 0x23`, wr.h:250-251), and that route would not set the latch. | None for levels <= 7 (the header guard is unchanged). Before the next level adds a store after the release: in `sw3_wr()`, for a CA7 `RESET_CTL` value with bit 0 clear, read the live IOCTL and refuse unless it has CPUHALT or (`l3 >= 7 && rv_done`); set `released` in the second case; add it to the glue sweep. |
| F2 | L | The latch refuses BAR2 stores only. With `released = 1` the glue still accepts SYS_MEM wrapper `RESET_CTL`/`IOCTL` and the SYS_MEM bank index (glue:250, 262; observed: probe P1b returns 0). A SYS_MEM reset under a running firmware changes chip RAM state without a BAR2 store. The header cannot issue one today (`SWT_SM_*` are once-only tuples of row 4, wr.h:236-241, read from source). | None for levels <= 7. Recommended with F1: refuse `SWW_WIN_SMWRAP` and `SWW_WIN_SYSMEM` stores while `released`. |
| F3 | L | The mutant `identity: reset-vector tuple ignores the board (N3)` (mut:675) is listed EQUIVALENT but is not. A direct `swl_wr(&x, SWW_BAR2, 0, 0, iw(0))` after a level-5 run with `x.board` set to the other board returns -21 and stores nothing on the shipped header, and returns 0 with one store on the mutant (observed: probe P2). The reason given (the pin runs first in `swl_stage3`/`swl_w14`) is true for the step functions only; the board term of the `SWT_RV` clause (wr.h:852) is therefore not pinned by a test. | Add that direct-call case to `rund()` and change the entry to `None`. Test-only; not needed for a run. |
| F4 | L | The glue half of the board binding is not executed by any host test. `sweet_wlan_stage3()` is only text-audited (`GLUE_ASSIGNS`, mut:130-135), and the audit strips string literals, so `"/"` and `"model-number"` (glue:325) and the condition of `x.img = NULL` (glue:337) are not pinned. For levels 4-5 this glue line is the only board check (the header pins the identity from level 6, wr.h:1722-1729). Every failure of this code gives board 0 or an unknown image, which stores nothing (observed for the logic: probe P3). | Optional: pin the two literals and the `if` text in `audit_store_path()`. |

Notes (no fix asked):

- N1: `glue_after()` in the mutant tool omits patch 0149, which also changes `sweet_wlan.c`. The audits therefore do not see `sweet_pcie1_mmu_probe()`. That function is not in this range, has no store (glue:362-385, read from source) and was reviewed in `0148-0153-*`. The diff between the tool's glue and the built file is exactly the 0149 hunks (observed, `diff`).
- N2: `released` is set at the first accepted IOCTL store without CPUHALT, which is `0x3` while the core is still in reset (wr.h:295), and it is never cleared in a boot, also not after the CA7 restore. Both are on the safe side.
- N3: patch 0151 is still being prepared and will apply before 0154. If it changes `sweet_wlan.c` or `sweet_wlan_wr.h`, the 0154 hunks and this review need a re-check against the new context.
- N4: the three citations I could open match (observed): `SICF_CPUHALT (0x0020)` at `include/siutils.h:306`, `MAX_READ_TIMEOUT 2 * 1000 * 1000` at `dhd_pcie.c:598` (DHD 865527e), `rings_info_ptr` at `include/bcmpcie.h:384`, the bwfm poll at `if_bwfm_pci.c:869-875` and range check at `:880-881` (9d8cbe8); root lookup at `pexpert/gen/device_tree.c:346-350`.

## Per-requirement verdicts

### 1. Release gate: PASS (F1 is the uncovered route)

- glue:255-257 refuses `space == SWW_BAR0 && win == SWW_WIN_CA7WRAP && off == SWL_AGENT_IOCTL && !(v & 0x20)` when `h->l3 < 7 || !h->rv_done` (read from source). `SW_CPUHALT` is `0x20u` (glue:43).
- `win` is bound to the live cfg 0x80 before the store (glue:266), so the gate cannot be passed by naming another window (read from source).
- `rv_done` is set only after the one store at BAR2 window 0 offset 0 (glue:274-276) and cleared at stage-3 start (glue:330). `l3` is at most 7 (glue:408) and is 5 when the PCIe core rev check fails (glue:472-475) (read from source).
- The SYS_MEM wrapper is untouched: the gate names `SWW_WIN_CA7WRAP` only. CPUHALT values (`0x21`, `0x23`, `0x20`) pass at every level.
- Observed: the glue sweep (9 levels x `rv_done` x 8 values x 2 wrappers, mut:907-930) passes, and the five gate mutants (mut:1042-1046) are killed (mutant run below).
- Observed: levels 3-6 store only `0x23` and `0x21` to the CA7 IOCTL, level 7 stores `0x3` and `0x1` after W14; the glue refused 0 stores in 10 full runs (probe).

### 2. Latch: PASS (F2 is the scope limit)

- Set at glue:269-271, after every refusal and directly before `*p = v` (glue:272); condition is the gate's value test on the CA7 IOCTL only.
- Checked at glue:258-260: every `SWW_BAR2` store returns -1 while `released`. Cleared only at glue:326, at each stage-3 start; `sweet_wlan_stage3()` runs at most once per boot (glue:401-404, 476-478).
- Still possible while `released`: cfg 0x80 and 0x84, CA7 `RESET_CTL` (the restore row wr.h:338), CA7 IOCTL with CPUHALT (observed in the sweep, mut:949-957).
- Bypass search (read from source): `h->tcm` is written through only in `sw3_wr()`; `sw3_rd()` reads; `sw3_map2()` only maps. `sw3_wr` is used once, as the bus callback (mut:240 audit, passes). The restore table has no BAR2 row (wr.h:335-340), so no restore is refused by the latch. Stage 1 (`sw_rd`) writes only cfg 0x80 and runs before stage 3.
- Observed: after a full level-7 run through the real glue, `released = 1`, a BAR2 store to window 0 and to the tail window both return -1; the fake counted 0 BAR2 stores after the release.

### 3. Board binding: PASS (F4 is the test gap)

- Rows carry the board (wr.h:97-101); `swl_fw_known()` compares length, CRC and board (wr.h:103-115).
- `swl_board_id()` (wr.h:117-138): NULL gives 0; a length other than 8 / 7 is skipped before any byte is read; the compare reads exactly `n` bytes of both strings, the literal's NUL included. Accepts exactly `"bluejay\0"` (8) and `"oriole\0"` (7). Observed in the host test (test:2231-2234: wrong length, prefix, longer, other case, NULL, zero length, trailing bytes all give 0) and by the three `board id` mutants (killed).
- Device-tree read (glue:325): `&&` binds before `?:`, so `swl_board_id()` is called only when both calls returned `kSuccess`; `root` is not used otherwise; `mn`/`mnsz` start as NULL/0. `SecureDTGetProperty` checks the property against the device-tree bounds (`device_tree.c:485-499`) and returns the stored length; at most 8 bytes inside the property are read (read from source).
- The producer writes the name with its NUL: `cstr('bluejay' if board == 'sweet' else board)` (`tools/mkadt.py:488`); `tools/test_mkadt_board_identity.py` passes with `b"oriole\0"` (observed).
- Use: glue `rv_ok` (glue:329) and the level >= 4 hand-over (glue:337-340); header `swl_fw_pin()` (wr.h:1445) and the `SWT_RV` clause (wr.h:852).
- Observed (probe P3, oriole image, boards 0, 1 and 3): levels 4-7 return -21 at row 8 with 0 stores and 0 reads; level 3 (no image involved) runs as before. Observed in the host test for the header alone at levels 6 and 7 (test:2259-2273).

### 4. Levels <= 7 unchanged on the right board: PASS

- Observed: my probe builds the host test at `7cfcbf7` with the old header and old `sw3_wr`, and at `a370142` with the new ones, puts the real `sw3_ptr`/`sw3_wr` in front of the fake bus, and runs levels 3-7 for the bluejay and the oriole identity. Store traces (every non-bulk store, a hash and count of the bulk BAR2 stores), return codes, flags and the captured log are identical (`diff`, 1126 lines). Store counts: 37 / 441045 / 441085 / 441086 / 441097 (bluejay), 37 / 441063 / 441103 / 441104 / 441115 (oriole).
- The header's log text changes only in the refusal line of `swl_fw_pin()`. The glue adds the line `stage 3 board N ...` (glue:328) and extends the refusal line (glue:338) (read from source).

### 5. Waits bounded: PASS

- No wait is added or changed. New loops: wr.h:109 (2 rows), wr.h:127 (2 names), wr.h:131 (at most 8 bytes). The W16 poll is still `SWW_FWUP_POLLS` = 100 (wr.h:1575) (read from source).

### 6. No EP Command write, no S2MPU/SysMMU, no new register: PASS

- The patch adds no store and no mapping. `sw3_wr()` still refuses every config offset but 0x80 and 0x84 (glue:249). `SW_CPUHALT` is a value mask, not an address. My probe counted 0 stores to cfg 0x04 in all 10 runs (observed); the host test checks the same count in run E (`cfg04_writes == 0`, test:2360-2364).

### 7. Tests: PASS with F3

- Old and new mutant lists compared by script (observed): 375 -> 390 header mutants, 37 -> 47 glue mutants, no old name missing, exactly 9 entries changed, all of them only in the pattern text that 0154 changed (`swl_fw_known(..., x->board)`, `&& board`, the row text, the anchor after `swl_fw_known`). Each still removes the same guard; the mutant run passes, so all 9 are killed.
- EQUIVALENT `run E swl_left: the run E tail branch dropped (G4/Y5)` (mut:670): agreed. `x->w84` becomes the tail window only after `swl_wr()` armed `SWW_RG_W84` (wr.h:886, 1539-1540); the group is disarmed only together with `x->w84 = 0` (wr.h:1541-1543, 1555-1556) or by `swl_done()`, which W16/W17 call only after `swl_w84_zero()` (wr.h:1604, 1669, 1678, 1700) (read from source).
- EQUIVALENT `identity: reset-vector tuple ignores the board (N3)` (mut:675): not equivalent, F3.
- G3 cases test what they say (read from source, and each mutant is killed): X3 test:2524-2536, X8 test:2537-2543, X9 test:2544-2547, X15 test:2548-2549, X2 test:2519-2521, X11 test:2420.
- G6 cases: link loss with the console in the tail window (test:2463-2464: restore skipped, report says `TAIL WINDOW (restore not done)`); a refused read at each of the first 60 reads of W17 with everything above 0x400000 (test:2550-2569: cfg 0x84 back at 0, no BAR2 store, report never says a wrong window).

## Commands run

| command | result |
|---------|--------|
| `cd shim && PIXELIOS_HOST_TESTS=always ./build.sh --board oriole --avb --pstore-fast --usb-gadget --out out/w7-review` (pixos-run.sh, heavy lock) | exit 0; `RESULT: PASS wlan_wr_host_test`, `RESULT: PASS wlan_xnu_host_test`, `RESULT: PASS wlan_nv_host_test`, `RESULT: PASS wlan_fw_host_test`; 296 `PASS` lines, no failing check |
| `python3 -B tools/test_wlan_wr_mutants.py` (pixos-run.sh, heavy lock) | exit 0; `Ran 8 tests in 1244.732s`, `OK` (slow because another build shared the machine). The run passes only when every header mutant not listed EQUIVALENT (383 of 390) and every glue mutant (47) is killed; it prints no per-mutant lines |
| `tools/patch-numbers check` | exit 0; `RESULT: PASS this tree's patch numbers do not overlap other branches or reservations` |
| `python3 -B tools/test_mkadt_board_identity.py` | `Ran 4 tests ... OK (skipped=1)` |
| my probe (scratch, not committed): old vs new header + glue, levels 3-7, two boards | traces and logs identical; 0 glue refusals; BAR2 after release refused |
| my probes P1, P1b, P2, P3 (scratch, not committed) | as quoted in F1, F2, F3 and requirement 3 |

`git status --short` in the worktree was empty before the runs and showed only this review file after them, HEAD `a370142` both times, so the runs used the committed tree (observed).

## Not verified

- Nothing was run on the phone. That the gate and latch behave the same on the chip is a guess from the code; they do not depend on chip behaviour except the live cfg 0x80/0x84 reads that were already there.
- I did not build the kernel. An object `sweet_wlan.o` built from the patched source exists in the author's local build and contains the new strings (observed with `strings`); I did not check that build for warnings.
- That the ADT handed to XNU on the phone carries `model-number` = `"oriole\0"` (7 bytes): read from `mkadt.py` and its unit test; the patch cites run `o3-w5-l7`, whose log I did not open. If it is anything else, levels 4-7 stop before the first store.
- F1's power-on state (IOCTL 0, RESET_CTL 1) is read from the host test's header comment, not from a log.
