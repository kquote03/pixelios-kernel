# Review: sweet-wlan stage 3 patches 0129 / 0131 / 0132 (independent, different model family)

Reviewer: Claude Fable 5.2 (delegate, worked alone, read-only; the author family of these patches is not Fable). Date 2026-11-14.
Requested by the 2026-11-14 oriole plan, step W5a (rule 5). Driver: Claude Opus 5.5. Stored verbatim in substance; line refs are `patch:line` in `xnu/patches/`.

**Verdict: SAFE WITH FIXES** (F1 before running `sweetwlan=5|6`; F2 before the branch leaves `wip/*`; F3 needs no code change to be safe, but any constant bump needs a new review)

- `sweetwlan=3` (0129): safe to run once levels 1-2 have passed on the same fresh `--wlan-hold` boot.
- `sweetwlan=5` (0131), `sweetwlan=6` (0132): nothing can write outside the pinned windows or release the ARM core; F1 should go in first.
- On the Pixel 6 (oriole) as it stands: levels 4-6 stop before W0 with zero stores (F3).

How checked: all six patches (0124, 0128-0132) and docs 37/39/40/42 read; stack applied to a scratch tree; `shim/wlan_wr_host_test.c` and `shim/wlan_nv_host_test.c` built against it with clang ASan+UBSan (both PASS); a 60 000-case ASan fuzz of `swl_nv_dhd()` on exact-size heap buffers (clean, max varsize 27068).
Not verifiable: every DHD line citation (`~/pixelios-artifacts/refs/wlan-stage*` absent on this machine); `docs/research/43`/`43a`, `docs/reviews/0130-*`, `REVIEW.md` absent from the checkout.

## 1. Single deny-by-default writer, pinned windows: PASS (two notes)
- Store sites: `*p = v` in `sw3_wr()` (0129:123) and the stage-1 BAR0 window select in `sw_rd()` (0124, reviewed). `sw3_wr` is reached only via `swl_store()` (0129:527-532), called only by `swl_wr()` (0129:638) and `swl_restore()` (0129:562, 568).
- Tuples in `swl_wt[]` (0129:325-356, 0131:200-209, 0132:156) are constants except four derived kinds recomputed inside `swl_wr`: bank index (nrb/nb pinned 8/21, coreinfo `& 0x3FF == 0x115`, 0129:340-341, 616); image words (CRC-checked, ascending, inside `[0x200000, SWW_IMG_END)`, 0130:191, 313-316); tail words (`swl_geo_get`/`swl_tw_word`, 0131:467-476); reset vector (image word 0 of the pinned firmware, 0132:181).
- Glue `sw3_wr` refuses ISO (PMU) and ELBI (0129:107); ATU only region 2 +4/+8/+0x10/+0x14; config only 0x80 and 0x84 (0/0x400000); BAR0 only IOCTL/RESET_CTL of the two wrappers and SYS_MEM bankidx; BAR2 only RAM/tail (0132:39-42); `sw3_ptr` bounds offsets (0129:67-84).
- No PMU/PMIC/CMU/GPIO write; SRPWR read-only at W3 (0129:768-782).
- Note L1: the glue does not bind values for cfg 0x80/ATU/BAR0 (table does). Note: stage-1 window select and `swl_rs[]` restores are table-driven exceptions by design.

## 2. Bounded loops and waits: PASS
`swl_wait` <= 30 polls (0129:684-701); bank loop nb = 21 (0129:852); image copy from `img_len <= 1764018` (0130:417-433); NVRAM loops `len <= 27068` (0131:257-289); `swl_wr_pick` forward only (0131:431-437); `read_random` non-blocking (0014:136-139). Residual since 0124: one stalled MMIO access has no timeout.

## 3. Stop and restore: FINDING (F1, F4)
`swl_stop` latches and restores once (0129:576-586); restores read back; `restore=failed|skipped|ok|none` printed (0129:569-572, 922); BAR0 restores skipped when preconditions fail (0129:549-557). W84 group armed across W8/W9 (0130:383, 402) and W11-W13 (0131:638, 656, 683); no two groups co-armed.
- F1 (M): the cfg 0x84 restore (0130:208) is not guarded by the link/ATU-r0 preconditions (only BAR0 is, 0129:549): a stop for "link not L0 / PHY isolated / ATU r0 changed" during W8-W9 or W11-W13 still issues config TLPs. Fix: run `swl_pre_link` before a `SWW_CFG` restore, report `skipped`.
- F4 (L): a fault is a panic in XNU (no probe read), not stop + restore.
- After a STOP in W3..W14: region 2 stays enabled (aimed at BAR2's own range), SYS_MEM out of reset, CA7 halted, cfg 0x80 on the last window. Harmless; not visible in `restore=`.

## 4. Container inputs, arithmetic, NVRAM port: PASS
Container offsets/lengths checked, CRC per entry, recomputed before W0, W8, W10, W14 (0130:351-369). `SWW_IMG_END = 0x3AEAB4` below the 4 MiB map; `ram_bytes >= SWW_RAM_MIN` (0129:865). `swl_geo_get` (0131:321-334) bounds; static asserts (0131:142-143). NVRAM scratch input + 8, max index len + 6 (fuzzed). `swl_nv_process` (0131:251-291) matches `process_nvram_vars()`. `vb[len-1] = 0` (0131:307) agrees with the repo's own reading (doc 39:50); content-only.

## 5. Opt-in levels: PASS
No boot-arg / 0: returns before any access; > 6 refused (0132:89-90). Levels 1-2 add no MMIO access (0129:189). Stage 3 gated per level (0129:604), cumulative (0132:255-286); level 6 falls back to 5 unless PCIe core rev < 74 or 128 (0132:98-101). Higher levels can only stop earlier.

## 6. Can 0132 release the ARM core: PASS (wording correction)
W14 is one BAR2 store (0132:156, 241). Release needs CA7 IOCTL 0x3 then 0x1 (doc 37:92); the table has only 0x23/0x21 for the CA7 window (0129:343-349); RESET_CTL := 0 needs companion IOCTL == 0x23 (0129:346-347). Correction: since 0129 W6 the core is out of reset and held by CPUHALT (IOCTL 0x21), verified by read-back only (doc 40 U-N1). EP bus master is off before every guarded write: no DMA into host memory.

## 7. Constants not traceable in this checkout: FINDING (F2)
Missing: docs/research/43, 43a, docs/reviews/0130-*, review 0125/0132 notes, REVIEW.md. Resting only on them: `SWW_RAM_MIN` 1791226, `SWW_RAM_VARS` 27208, `SWW_NV_VARS_MAX` 27068, `SWW_IMG_MAX` 1764018; `SWW_FW_LEN` 1764018 / `SWW_FW_CRC` 0x1153ccff ("run wd3", only in tests); RAM end 0x4a0000 (host test only); `dhd_pcie.c:11735-11745`, `include_pcie_core.h:1161`, `include_bcmrand.h:50`, `bcmutils.c:5101-5146` (no DHD source here). Traceable to docs 36/37/39/40/42: cfg 0x88, wrapper offsets/reset values, SRPWR bits, coreinfo 0x115, bankidx/bankinfo, RAM base, 4 MiB cap, BAR2 window rule, 0xFEEDC0DE, 30 x 10 us waits.

## 8. Hang, kernel memory, hardware damage: no blocking finding
No DMA; `sw_nvbuf` static 27076 B; stack objects synchronous; BAR2 mapping permanent. A stalled MMIO access hangs the phone (no watchdog). Bulk words re-check preconditions per 64 KiB, caught per store by the live cfg 0x84 read. Nothing reaches regulators, RF, OTP, SPROM or the chip PMU. cfg 0x88 coherency bits (0x300) never written: functional risk only. NVRAM entry not pinned (0132:115). Possible spurious (safe) stops: 0xFFFFFFFF from uninitialised SRAM, ASPM L1.

## Findings
| # | Sev | Finding | Fix |
|---|---|---|---|
| F1 | M | cfg 0x84 restore without link/ATU-r0 re-check (0130:208, 0129:549) | `swl_pre_link` before a `SWW_CFG` restore; report `skipped` |
| F2 | M | design doc 43/43a and cited reviews not in the repo; five constants without in-repo source | commit the docs or re-cite before leaving `wip/*` |
| F3 | M (process, fail-safe) | pins are bluejay observations; the oriole container has FW length 1764091 (> `SWW_IMG_MAX` 1764018) and CRC 0x92567396 (not 0x1153ccff) | none for safety: on oriole levels 4-6 stop before W0 with no store; changing the pins is a new hardware-write change needing its own review. Level 3 is the first time bluejay pins (cfg 0x88, coreinfo 0x115, rev 72) meet oriole hardware; each is a live stop-gate |
| F4 | L | fault = panic | document |
| F5 | L | state left after a STOP past W2 not in `restore=` | print it |
| F6 | L | glue does not check values for cfg 0x80/ATU/BAR0 | add the CA7 IOCTL bit-0x20 check and pin values in the glue |
| F7 | L | "core stays in reset" premise wrong since W6 (CPUHALT) | fix wording |
| F8 | L | `vb[len-1]` vs `bufp[len]` (from memory) | re-check against source |
| F9 | L | W4 resets SYS_MEM unconditionally; W6 not gated on the CA7 entry state | gate on observed power-up state |

## Driver decision (2026-11-14 plan W5a)
- Level 3 (0129): SAFE -> may run after W2-W4 pass on oriole.
- Levels 5, 6 (0131, 0132): SAFE only with F1 fixed; F1 not implemented in this run -> REVIEW-PENDING (fix needed). In addition F3 makes levels 4-6 stop before W0 on oriole (firmware length/CRC pins are bluejay's), so they cannot do anything here until a reviewed re-pin.
