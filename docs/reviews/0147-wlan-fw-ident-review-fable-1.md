VERDICT: SAFE

# Review: xnu patch 0147 sweet-wlan-fw-ident (independent, different model family)

Reviewer: Claude Fable 5.2 (delegate, worked alone, read-only, no phone access). Author family: Claude Opus. Date 2026-11-14.
Range `6b1da78..96164e1` (one commit) on `wip/oriole-next`. Patch `xnu/patches/0147-sweet-wlan-fw-ident.patch` on top of 0124, 0128-0132.
Line refs: `wr.h:N` and `glue:N` are lines of `bsd/dev/sweet_wlan_wr.h` and `bsd/dev/sweet_wlan.c` after applying 0124, 0128-0132, 0147 to an empty tree; `0147:N` is a line of the patch file.

No finding blocks running `sweetwlan=4|5|6` on oriole. The findings are L (recommendations) plus the carried process finding F2 of the earlier review (needed before the branch leaves `wip/*`).

## What I ran (all on the host)

| Check | Result |
|---|---|
| `hostwr.sh REPO OUT 0147` (gcc `-O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined`, `shim/wlan_wr_host_test.c` against the header after 0129-0132 + 0147) | `RESULT: PASS wlan_wr_host_test`, no sanitizer output |
| Header used by the test vs header from `git apply` of the seven patches | byte-identical (`cmp`) |
| `zlib.crc32` of `~/pixelios-artifacts/wlan-fw/files/fw_bcmdhd.bin` (read in place, not copied) | length 1764091, crc32 `0x92567396` |
| `sha256sum` of the same file | `8a52ffe1d71d29f3729247a31be912ff9e9b9959effa36fe94f2e3f5cccf916b` |
| `python3 -B tools/test_wlan_wr_mutants.py` | `Ran 8 tests in 632.968s`, `OK` (every mutant killed, including the new fwid / F1 / F5 ones) |
| Run logs `logs/runs/20261114T093743Z-wlan-oriole-l3`, `...T094404Z-wlan-oriole-l4` | `W5 RAM 0x2a0000 bytes, ram_end 0x4a0000`, `coreinfo 0x115 nrb 8 nb 21`, `level 3 done ram_end=0x4a0000 banks=21`; `fw container entry FW   length 1764091 offset 0x40 crc32 0x92567396 ok`, `W8 no usable image (1764091 bytes, at most 1764018)`, `stopped at W8 restore=none` |

## 1. Single deny-by-default writer: PASS

The patch adds no store site. Store sites after the patch (unchanged in number):
- `*p = v` in `sw3_wr()` (glue:253), reached only through `swl_store()` (wr.h:658-662).
- `swl_store()` callers: `swl_wr()` (wr.h:824) and `swl_restore()` (wr.h:702 BAR0 window select, wr.h:708 the restore word).
- Stage-1 window select in `sw_rd()` (0124, not touched).

What the patch changes on those paths:
- wr.h:789 (`SWT_RV`, W14): `img_len == SWW_FW_LEN && img_crc == SWW_FW_CRC` became `swl_fw_known(len, crc)`. One more accepted identity (oriole).
- glue:302 `rv_ok` the same way; the glue's independent W14 rule (glue:245: level 6, `rv_ok`, once, `v == h->rv`) is otherwise unchanged.
- wr.h:679-688: the cfg 0x84 restore store is now additionally gated (F1). Stricter.
- Table bounds that follow `SWW_IMG_END` (0x3AEAB4 -> 0x3AEAFC): `SWT_IMG.hi` grows by 18 words (wr.h:236), `SWT_LAST_LO.off` rises by the same (wr.h:230). Both stay in BAR2 window 0 below 0x400000.
- No new tuple in `swl_wt[]`, no new row in `swl_rs[]` or `swl_rt[]`, no new space. `sw3_wr` still refuses ISO and ELBI (glue:237).

## 2. Constants: PASS, one carried finding (N4 = F2)

| Constant | Source | Status |
|---|---|---|
| `SWW_FW_LEN_OR` 1764091 | file size on the host; container line of run wlan-oriole-l4; `device/google/oriole/docs/PORT.md:41,57` | traced, checked myself |
| `SWW_FW_CRC_OR` 0x92567396 | my own `zlib.crc32` of the file; same run log line | traced, checked myself |
| sha256 prefix `8a52ffe1` in the comment | my `sha256sum` | matches |
| `SWW_FW_LEN_BJ` / `SWW_FW_CRC_BJ` 1764018 / 0x1153ccff | unchanged values of 0132 ("run wd3"); doc 39 gives the bluejay size | length traced, CRC only by the comment (as before, F2) |
| `SWW_RAM_MIN` 1791299 | 1764091 + `SWW_RAM_VARS` 27208, static assert wr.h:66 | arithmetic traced; 27208 itself is not (below) |
| `SWW_IMG_MAX` 1764091, `SWW_IMG_MAXW` 441023, `SWW_IMG_END` 0x3AEAFC | (1764091+3)/4 = 441023; 0x200000 + 4*441023 = 0x3AEAFC; static assert wr.h:66 | derived, checked |
| observed RAM 2752512 B / ram_end 0x4a0000 | run wlan-oriole-l3 log | traced |

Not traceable in this checkout: `SWW_RAM_VARS` 27208 and `SWW_NV_VARS_MAX` 27068 cite `docs/research/43 s.2`, which is still absent (earlier F2). The patch does not change them, but the new `SWW_RAM_MIN` is built on 27208. The budget was derived from the bluejay cal text (27062 B); the oriole cal is 24552 B (PORT.md:41), so it fits.

## 3. Loops and waits: PASS

- `swl_fw_known` (wr.h:99-110): `sizeof swl_fwids / sizeof swl_fwids[0]` = 2 iterations, no early-exit dependence on input.
- `swl_restore` (wr.h:672-715): 4 rows of `swl_rs[]`; `swl_lpre` is evaluated at most once per restore (`lpre` cache) and does 16 RC-local reads (wr.h:613-621).
- `swl_left` (wr.h:719-730): no loop, no bus access.
- Image loops grow from 441005 to 441023 words (wr.h:1114-1117, 1218-1232), still bounded by `img_len <= SWW_IMG_MAX`.
- No new wait. Residual as before: one stalled MMIO access has no timeout.

## 4. Stop and restore: PASS for the restore, FINDING N2 (L) for the new report line

- `swl_stop` (wr.h:733-745) latches once, records `armed_at_stop`, runs `swl_restore`, then `swl_left`.
- F1: for a `SWW_CFG` row the restore first runs `swl_lpre` (PHY isolation word, ELBI 0x2C8 L0, ATU r0/r1; all RC-local, no TLP). On failure (including a failed read, code 8) it prints `restore of group 8 skipped, link precondition N`, sets `skipped`, sends no store and no config read (wr.h:679-688). Result `restore=skipped` (wr.h:715). Host test `f1_cfg_restore` covers link-up (restored, read back) and link-down (skipped, `f.nw` unchanged); the four F1 mutants are killed.
- BAR0 restores were already gated by `swl_wpre`, which now calls `swl_lpre` first (wr.h:633); same reads in the same order as before, so nothing changed there.
- A successful restore is still stored, read back and printed `ok`/`FAILED` (wr.h:708-713); `rs_ok` records the read-back ones.

State left after a stop (all volatile, inside the chip or the RC):

| Stop in | Left changed |
|---|---|
| before W0 (image / pin / NVRAM pre-checks) | nothing; no stage-3 bus access |
| W2 | ATU region 2 base/limit/target written; CR2 disabled again if the restore ran |
| W3..W7 | region 2 enabled; cfg 0x80 on the last window; SYS_MEM and CA7 as far as the reset sequences got (restore re-asserts reset only for the step that failed) |
| W8/W9 | as above, SYS_MEM out of reset, CA7 out of reset with CPUHALT; cfg 0x84 = 0 (restored) or 0x400000 (skipped, link down); possibly the last RAM word = 0 |
| W10 | as above, cfg 0x84 = 0; part or all of the image in chip RAM |
| W11..W13 | image in RAM; part or all of NVRAM text, token, rTLV; cfg 0x84 = 0 (restored) or 0x400000 (skipped) |
| W14 | all of the above; possibly the vector word at backplane 0 |

Is the "left after stop" line correct? For every stop that happens between steps or after a completed tuple: yes (checked each branch of wr.h:724-727 against `swl_done` at wr.h:870-876 and the arming at wr.h:823, 947-960, 1175-1204, 1311-1374). Gaps (N2):
- a. A stop inside the store or read-back of `SWT_ATU_CR2`, `SWT_SM_ASSERT` or `SWT_CA_ASSERT` prints `untouched` for that item, because the `done` bit is only set after a good read-back (wr.h:849) while the store was already issued (wr.h:823-826). If the restore then fails or is skipped, the real state is unknown, not untouched. The `armed 0x..` field and `restore=` on the next line still show it.
- b. A stop at the `SWT_SM_RELEASE` / `SWT_CA_RELEASE` store whose read-back read fails, with the restore skipped, prints `in reset`; the real state is unknown.
- c. `cfg84=0` is printed for stops before W0 has read cfg 0x84 (wr.h:916); it is an assumption there, not an observation.
- d. The line does not mention chip RAM contents (image words stored, last word, NVRAM/token/rTLV, vector) or the wrapper IOCTL values (0x1 / 0x21). `tuples` shows only ONCE tuples; `img_next` and `tn[]` are not printed.
None of this affects a store; the line is informational.

## 5. Default off / levels 1-3: PASS

- No `sweetwlan` or 0: return before any access (glue:346-348), untouched.
- Levels 1-2: the patch touches only `sweet_wlan_stage3` in the glue (0147:14-32), which runs at level >= 3 (glue:411).
- Level 3: the image gate is `level >= 4` (glue:310), `rv_ok` needs level 6 (glue:302). No `SWW_CFG` restore group can be armed at level 3 (only run 2/3 tuples arm `SWW_RG_W84`), so `swl_lpre` adds no read in a level-3 restore. `swl_wpre` reads are identical in content and order.
- Two level-3 differences, both without hardware effect: (1) `SWW_RAM_MIN` is the W5 lower bound (wr.h:1065) and rose by 73; bank sizes are multiples of 8192 (wr.h:1059), so no reachable sum falls between 1791226 and 1791298 (218 x 8192 = 1785856, 219 x 8192 = 1794048). (2) every stop prints the extra `left after stop` line.
- Levels 4-5 are stricter than before: only a table identity is copied (previously any CRC-consistent image up to the bound).

## 6. External inputs and arithmetic: PASS

- Container: `swf_parse` bounds unchanged (`len <= 4 MiB`, `length <= len - offset`, CRC per entry; `sweet_wlan_fw.h:52-82`); the oriole container (1804688 B) parsed on hardware (run l4).
- `img_len` is at most 1764091 wherever it is used (wr.h:785, 1110); `4u * w + k` peaks at 1764092; `SWW_RAM_BASE + 4u * w` at 0x3AEAF8. No 32-bit overflow.
- `hw->rv` reads `sw_fw.p[0..3]` only when `rv_ok`, i.e. the length is a table length (glue:304).
- Static asserts hold at compile time: image bound (wr.h:66), tail bound (wr.h:77), tail window above the image (wr.h:78), larger pin = bound (wr.h:90).

## 7. Step-specific

**(a) Only a table identity is copied (W10) or used for the vector (W14) in the kernel build: PASS, recommendation N1.**
- W14: three independent checks: `swl_fw_pin` before the first store and again in `swl_w14` (wr.h:1380-1387, 1408, 1447-1454), `SWT_RV` inside `swl_wr` (wr.h:789), and the glue's `rv_ok` + `v == h->rv` (glue:245, 302-304). `swl_fw_known` compares length AND CRC of the same row (wr.h:104); mixed rows and neighbours are refused (host test `fwid`, six fwid mutants killed).
- W10: at level >= 4 the glue sets `x.img = NULL` unless `swl_fw_known(sw_fw.len, sw_fw.crc)` (glue:310-313). `swl_stage3` then stops in the pre-check `swl_img_check` (wr.h:1429-1436, 1110) before W0: zero stores, zero stage-3 reads. For a known identity, `swl_img_check` recomputes the CRC from the bytes before W0, at W8, W10 and W14, and W10 ends only if the CRC of the read-back equals the container CRC (wr.h:1234-1238).
- N1: this W10 guarantee sits only in the glue, which no host test compiles (the mutant audit only checks that the assignment `x.img = NULL;` exists, not its condition). The header alone still copies any CRC-consistent image up to the bound (wr.h:785, 1110). Correct by inspection today.
- Identity is length + CRC32, not a cryptographic hash (design of 0132, unchanged).

**(b) The new bound keeps every store inside the BAR2 RAM window; tail cannot overlap the image: PASS.**
- Image: backplane 0x200000..0x3AEAFB, BAR2 window 0, below `SWW_TCM_BYTES` 0x400000 (`sw3_ptr`, glue:207).
- Observed RAM (ram_end 0x4a0000): token at 0x49FFFC, vars at or above 0x499640, rTLV at or above 0x4995B8; all at or above `SWW_WIN_TAIL` 0x400000, offsets at most 0x9FFFC <= `SWW_LAST_HI_MAX` 0x1FFFFC.
- Any RAM size: `swl_geo_get` refuses a tail below 0x400000 (wr.h:460) and the image ends below it (static assert wr.h:78). For ram_end <= 0x400000 the last word is at or above 0x200000 + 1791299 - 4 = 0x3B553F > 0x3AEAFC, so `SWT_LAST_LO` cannot hit the image.
- RAM 2752512 >= `SWW_RAM_MIN` 1791299 and <= `SWW_RAM_MAX`.

**(c) F1 fixed as specified: PASS.** The earlier fix text was "run `swl_pre_link` before a `SWW_CFG` restore, report `skipped`". Implemented at wr.h:679-688 via `swl_lpre` (wr.h:606-622), reported per group and as `restore=skipped`. The BAR0 path's cfg 0x80 store inside the restore stays behind `swl_wpre`, which contains the same link half.

**(d) Nothing reaches PMU/PMIC/CMU/GPIO, regulators or the ARM core release: PASS.** No new tuple, space or offset. The PMU isolation word and ELBI are read-only (glue:237 refuses stores). The CA7 table values remain 0x23/0x21 (wr.h:213-219); no IOCTL value without CPUHALT exists. W14 is still one BAR2 store.

## 8. Worst case on the phone

- Hang: one MMIO access that never completes (no timeout; unchanged). Recover by holding power.
- Panic: a bus fault on an MMIO access is a kernel panic (earlier F4, unchanged).
- Damage: none found. All state the code can change is volatile: ATU region 2, cfg 0x80/0x84, two wrapper resets, and chip RAM with the CA7 halted. No flash, OTP, SPROM, regulator, RF or secure-world access. Endpoint bus mastering stays off (precondition), so no DMA into host memory.
- New worst case from this patch: the oriole image plus NVRAM tail plus the vector word sit in chip RAM with the core halted. Nothing executes them.
- A power cycle (Wi-Fi rail off) fully reverts it. Whether a warm reboot that keeps the Wi-Fi rail up clears chip RAM was not verified; the shim chain re-initialises the link either way.

## Findings

| id | Sev | Finding | Required fix |
|---|---|---|---|
| N1 | L | The "only a table identity is copied at W10" rule exists only in the kernel glue (glue:310-313), which no host test or mutant exercises; the header's `swl_img_check` / `SWT_BULK` (wr.h:1110, 785) accept any CRC-consistent image up to the bound | None before running. Recommended: also call `swl_fw_known` in `swl_img_check` (or the `SWT_BULK` clause) so the host test and mutants cover it |
| N2 | L | `left after stop` can be wrong in edge cases: `untouched` after a stop inside the store/read-back of ATU CR2, SM assert or CA assert (wr.h:724-726 vs 823-849); `in reset` after a release store whose read-back read failed with the restore skipped; `cfg84=0` before W0 has read it; chip RAM contents and IOCTL values are not reported | None before running (informational line; `armed`, `restore=` are right). Recommended: use `armed_at_stop` to print "store attempted, state unknown", print `img_next`, `tn[]`, and say "not read" before W0 |
| N3 | L | The identity table is not bound to the board: a bluejay accepts the oriole row and the reverse | None now (same chip, core never released). Bind the row to the board before any step that releases the CA7 |
| N4 | M (process, carried F2) | `SWW_RAM_VARS` 27208 / `SWW_NV_VARS_MAX` 27068 cite `docs/research/43`, absent from the checkout; the new `SWW_RAM_MIN` rests on 27208. The bluejay CRC 0x1153ccff is cited only as "run wd3" | Commit doc 43/43a or re-cite before the branch leaves `wip/*`. Not a blocker for the run |
| N5 | L (doc) | `SWW_RAM_MIN` is also the level-3 W5 gate, so "levels 1-3 unchanged" in the commit message is true only because bank sizes are multiples of 8 KiB | Say so in the comment at wr.h:36 |

Earlier findings: F1 fixed (7c). F5 implemented, with the gaps in N2. F3 resolved for oriole by the table. F2, F4, F6-F9 unchanged by this patch.

## What I could not verify

- Any behaviour on the phone: no device access by instruction. The level 4-6 runs on oriole with this patch have not happened yet.
- The kernel build of the patched tree (the glue is not compiled by any host test; I read it). An artifact `~/pixelios-artifacts/xnu-local-96164e1` exists but I did not inspect or rebuild it.
- The DHD line citations (`~/pixelios-artifacts/refs/wlan-stage*` not opened in this review) and `docs/research/43`, `43a` (absent).
- The bluejay identity CRC 0x1153ccff (no bluejay firmware file on this host path was checked).
- Whether a warm reboot clears chip RAM.
- Patch 0149 (`fa4999d`, read-only PCIe1 MMU probe) was committed on top during this review and also edits `sweet_wlan.c`; it is outside the reviewed range and was not reviewed.
- `docs/SAFETY.md` still names only the Pixel 6a / bluejay in its restore procedure; AGENTS.md rule 2 covers oriole. Not part of this patch.
