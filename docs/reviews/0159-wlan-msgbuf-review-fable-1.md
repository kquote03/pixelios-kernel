VERDICT: SAFE-AFTER-FIXES

# Review: xnu patch 0159 sweet-wlan-msgbuf (sweetwlan=8: msgbuf setup, bus master, first ioctl round trip)

Reviewer: Claude Fable 5.2 (independent, worked alone, did not write the patch, no phone access, nothing run on the phone, patch not changed). Date 2026-11-16.
Reviewed: `xnu/patches/0159-sweet-wlan-msgbuf.patch` at HEAD `3337859` (`wip/oriole-w8`) and the 0159 parts of `git diff a370142..3337859`.
Line refs: `glue:N` = `bsd/dev/sweet_wlan.c`, `wr.h:N` = `bsd/dev/sweet_wlan_wr.h`, both in `~/pixelios-artifacts/localbuild/work/w8/xnu/`; `test:N` = `shim/wlan_wr_host_test.c`; `mut:N` = `tools/test_wlan_wr_mutants.py`; `note` = `docs/research/51-wlan-msgbuf-host-dma.md`.
Sources: `bwfm.c/.h` = OpenBSD `if_bwfm_pci.c/.h`; `brcmf` = Linux v6.18 brcmfmac; `dhd` = `dhd-bcm4389-bluejay-a13`.
Tags: **[obs]** I ran it and saw it, **[src]** read from source, **[guess]**.
The header the host test extracts is byte-identical to the patched tree's `sweet_wlan_wr.h` and `sweet_wlan_chip.h` (`cmp`, [obs]); the `sw3_ptr`..`sw3_wr` text of the mutant tool's patch chain is contained verbatim in the patched `sweet_wlan.c` [obs].

Summary. The store path does what the note says. In every fault I could inject (each of the 61 run F stores failed, dropped or bit-flipped; each of the 1795 run F reads failed; hostile firmware data) the run ends with EP Command back at memory-only, or, when the link is down, reports truthfully that it could not write. Levels 3-7 are unchanged with the real `sw3_wr()` in the loop. The doorbell registers, message layouts, ring sizes and host_cap match the sources.
Two things are missing before the first run with bus mastering: the run never looks at the four host index-buffer addresses in ring_info, which are the only host addresses the firmware could use that the patch does not control (F1), and the note is wrong about the DMA address width of this SoC (36 bits, not 64), with no check on the pool address (F2). Both fixes are a few read-only lines in W18. F3-F5 do not affect safety but decide whether the first phone run can be interpreted; I recommend them before spending a run.

## Findings

| id | sev | finding | required fix |
|----|-----|---------|--------------|
| F1 | M | **ring_info host index addresses are not checked (rule R1 hole).** W18 reads all 15 ring_info words (wr.h:2065-2067) but uses only words 0-4 and the two counts; words 5-12 (`h2d_w/h2d_r/d2h_w/d2h_r_idx_hostaddr`, bwfm.h:174-181) are neither checked nor printed. The firmware advertises DMA_INDEX (flags 0x70050107). The host's only way to say "indices stay in TCM" is to leave these fields unset: brcmf writes them when it wants host indices (pcie.c:1273-1312) and writes nothing otherwise (pcie.c:1264-1272); DHD writes them only when `dma_*_ring_upd_support` is true (dhd_msgbuf.c:4320-4335) [src]. So the firmware decides from the field content [guess, firmware source not available]. If one of the eight words is not 0 at W18 (firmware default, stale RAM), the firmware would, after W22, DMA ring indices to a host address that is not in the pool. [obs] scenario Z15: with the words set to 0x1234000x the run passes, switches bus master on and prints nothing about them. That they are 0 on this firmware is [guess]; no run has printed them. | Required before the first level-8 run: in `swl_w18()` stop (no store) unless `f_ri[5..12]` are all 0, and print the eight words in the W18 line. Add the negative host-test case and a mutant. |
| F2 | M | **DMA address width: the note says 64-bit, the SoC does 36-bit.** note s.2 row 2 answers "Are 64-bit addresses normal here? Yes". DHD for this SoC says otherwise: "S.SLSI PCIe DMA engine cannot support 64 bit bus address. Hence, set 36 bit" (dhd `dhd_pcie_linux.c:2188-2197`, `DHD_PCIE_DMA_MASK_FOR_GS101 36` `dhd_pcie.h:825-828`, enabled in `Kbuild:892`) [src]. W18 only checks that `pool_pa + len` does not wrap 64 bits (wr.h:2057). An address with a bit at or above bit 36 would be truncated by the RC and the chip would write somewhere else in RAM. On oriole this cannot happen today: XNU RAM is 0x8_8000_0000 + 4 GiB (device/google/oriole/board.json:12-17), all below 2^36 [src]. The same DHD lines support the note's U1 for addresses below 2^36 (stock gives the chip plain physical addresses up to 36 bits). | Required with F1 (one line): W18 refuses unless `pool_pa + SWM_POOL_LEN <= 1ull << 36`; cite the DHD lines; correct note s.2. Note s.4 also says W18 checks "64 KiB aligned, inside XNU RAM": the code checks 16 KiB (`SWM_POOL_ALIGN 0x4000`, wr.h:422) and no RAM range; fix the note or the code. |
| F3 | M (function, not safety) | **A completion item that is not written yet is consumed and lost.** The firmware sets D2H_SYNC_XORCSUM (0x40000). DHD requires "either dma indices or d2h sync" (dhd_pcie.c:13857-13860) because with TCM indices the write index can be visible before the item's DMA has landed; DHD then waits on the item checksum [src]. Run F chose TCM indices and does no sync: an item whose type byte is still 0 is counted as "ignored" and the read index moves past it (wr.h:2276-2281). [obs] scenario Z13 (index first, bytes one poll later): `completion item type 0 ignored`, then `no completion ... has bytes`, stop TMO, bus master off. Safe, but the first phone run could fail for this reason and be read as a DMA or firmware problem. With a 10 ms poll the window is small [guess]. | Recommended before the run: treat type 0 as "not written": do not advance `f_r0`, re-read on the next poll (the 200-poll bound stays). Host-test case as Z13. |
| F4 | L | **Guard gaps are checked only at the end of a fully successful run** (wr.h:2346-2351). On every stop after W22 (timeout, bad completion, bad index) the gaps are not looked at, so "DMA suspect" cannot appear on the paths where a stray device write is most likely, and plan stop H10 ("memory corruption signs") gets no input from them. Rule R9 of the note is met only for the pass path. | Recommended: on a stop with `SWF_BM` set or `SWT_BM_ON` done, sync the pool and print the count of changed guard bytes (host memory only, no bus access). |
| F5 | L | **Two diagnostics the note promises do not exist.** note s.7 U2: "W4 probe line re-read after the run (FAULT_STATUS)": `sweet_pcie1_mmu_probe()` runs only before stage 3 (glue:546-548), so an S2MPU fault caused by the first device write is never read. U7: "the firmware console after W19-W23; a trap line stops the run": run F never re-reads the console. A firmware trap or an S2MPU fault would show only as `no completion`. | Recommended: call the (already reviewed, read-only) probe again after `sweet_wlan_stage3()` when `sweetpcie1mmu=1` and add that boot-arg to the run command; re-read the console (level 7 reader) on a run F stop. Otherwise remove the two claims from the note. |
| F6 | L | **Run command gaps** (docs/AUTOMATION.md:648-650). `--fail-on 'restore=failed\|DMA suspect\|bus_master=ON'` does not match `bus_master=unknown (store not read back, restore not done)` (wr.h:1007; [obs] Z8b prints it). If Command already reads 0x6 before W22 the run stops with PRE and leaves the bit, but prints no `bus_master` line at all ([obs] Z7: 43 of 43 cases; wr.h:1005 prints the line only when the tuple was reached). xrun applies its default panic patterns only with `--early` / `--stop-on` (tools/xrun:13-14); the documented command has neither, so a panic does not end or fail the run early [src, not tested]. | Add `bus_master=unknown`, `restore of group 16 skipped`, `panic\(cpu`, `Kernel trap at` to `--fail-on`. Optional: print the bus_master line at every level-8 stop from a live Command read when the link precondition holds. |
| F7 | L | **Unlisted unknown: the root port's own Command register.** The shim never writes the RC's DBI 0x04 (the only DBI write is 0x890, shim/wlanlink.c:371) and docs/research/36:147 lists RC Command / bridge window as unverified. A root port with Bus Master Enable clear answers upstream memory requests with Unsupported Request (PCIe base spec, [guess] for this DesignWare core). Outcome if so: requests dropped, `no completion ... is untouched`; nothing worse. The note's s.7 does not list it, and the planned conclusion "DMA blocked: Wi-Fi ladder ends" would be drawn without having looked at it. | Add as U8 to the note with the DBI 0x04 value the shim's row-24 read already prints; no code needed for safety. |
| F8 | L | **Mutant list: three "EQUIVALENT" claims are wrong and several rules have no mutant.** (a) `short MAC answer accepted` (mut:363) claims no fake case gives a short answer with status 0; `f.fw_len = 3` does: the shipped header stops with MSG, the mutated header returns 0 and prints the pass line with a zero MAC ([obs] Z5b on both headers). (b) `bus master tuple needs no W21` (mut:341): a direct `swl_wr(CFG, 0x04, 0x6)` at level 8 on a finished level-7 state returns `-SWW_E_PHASE` with no store on the shipped header; on the mutated header it returns 0, makes one store and the fake's Command reads 0x100006 ([obs] Z14 on both headers). Same pattern as review 0154 F3. (c) `sync callback not required` (mut:334) is killed by a crash, not equivalent. The suite accepts a killed "equivalent" silently (mut:1336-1341), so a wrong label is never noticed. No mutant covers: the `cmd` compare and `\|\| got` of the completion check (wr.h:2263), the printable filter (wr.h:2341), any `swm_sync()` call, the address fields written into ring items (wr.h:2222-2223, 2242-2243), the `omax` bound of the copy (wr.h:2305), the ring-full check (wr.h:2176), the 64-bit wrap term (wr.h:2057). | Test-only: add Z5b and Z14 as cases and set both entries to `None`; make a killed `EQUIVALENT` mutant a failure; add the missing mutants (at least the ring-item address fields and one `swm_sync` removal). Not needed for the run. |
| F9 | L | **The glue's independent gate for chip RAM after the release is a count only.** At level 8 `sw3_wr()` accepts any RAM word with any value, 96 times (glue:272, 279). Which word and which value is decided by the header alone (`swl_f_kind()`, wr.h:730-795). The damage of a header bug is bounded to chip RAM (a power cycle), and the host addresses that matter are also checked by the fake's out-of-pool counter in tests. | None for this run. If run F grows, give the glue the pool range so it can refuse a store whose value is a host address outside the pool. |

Notes (no fix asked):

- N1: the pass-path log says `run F stored 39 chip-RAM words`; when ACK and completion arrive in different polls the count is 40 or 41. Budget 96 is far above both ([obs] G1: 39; G4: a firmware that moves the index on every poll hits the budget at 96, the glue refuses the 97th store, the run stops and bus master goes off).
- N2: the note's s.4 lists `ver` first; the code asks `cur_etheraddr` first (wr.h:2331, 2339). No effect.
- N3: `swl_f_kind()` allows the rxpost write index and the tx/rx-complete read indices any value below 1024 (wr.h:778-793); run F only writes 0 there. Wider than needed, chip RAM only.
- N4: bwfm and brcmf, with DMA_INDEX set, use host index buffers (bwfm.c:612-666, brcmf pcie.c:1273-1312); TCM indices with DMA_INDEX set is the path brcmf takes only after an allocation failure and DHD takes after one (dhd_pcie.c:13941-13947, 13956-13962). It is a legitimate path in all three hosts but the least exercised one [src]. Together with F1 this is the firmware behaviour I trust least.

## Per-item verdicts

### R1 Pool only: PASS for every address the patch writes; F1 for the addresses it leaves in place

- The only place a 64-bit host address is formed is `swm_pa()` (wr.h:718-725): `pool_pa + off`, 0 unless pool, `pool_pa`, `pool_len == SWM_POOL_LEN`, `off <= len`, `len <= pool_len - off` [src].
- Uses: scratch and ring-update (wr.h:744, 754, 2148), the five ring bases (wr.h:766, 2130), the response buffer (wr.h:2212), the request buffer (wr.h:2226, `qlen <= SWM_L_REQ` at wr.h:2206). All offsets are compile-time constants (wr.h:421-444). Nothing read from the chip or passed by a caller enters an address [src].
- Chip-RAM words are accepted by `swl_wr()` only when `swl_f_kind()` finds exactly the layout value (wr.h:1081-1086); the two ring items carry `(uint32_t)pa`, `pa >> 32` of the same helper (wr.h:2222-2223, 2242-2243).
- The glue holds `pool_pa` only for the cache flush (glue:356) and hands it over once (glue:420-422).
- [obs] the fake counts every host address the firmware model follows outside the pool (`oob`, test:146-153); it stayed 0 in all my runs.
- Not covered: ring_info words 5-12 (F1); address width (F2).

### R2 Bus master: PASS

- One tuple sets it: `SWT_BM_ON`, once, needs W21, old low half must be 0x0002, arms group BM (wr.h:336-337). `swl_wpre()` also requires Command 0x0002 before it (wr.h:909). One tuple clears it at the end (`SWT_BM_OFF`, wr.h:338-339, called at wr.h:2353). The restore table has BM first (wr.h:371); a config restore needs only the RC-local link check (wr.h:948-956).
- W22-W25 are one function with no `swl_done()` before the end (wr.h:2315-2357), and the helpers clear only the W84 group, so group BM stays armed for every stop in between [src]. No path in `swl_w22()` or `swl_f_iovar()` returns non-zero without `swl_stop()` [src].
- The 32-bit store writes Status as 0: its error bits are write-1-to-clear, the rest read-only, so nothing changes; Command bits 3-15 are written 0 and were 0 (old low half 0x0002 enforced) [src, PCI spec from memory]. The shim's own Command store uses the same form (shim/wlanlink.c:322).
- Fault sweep (test:2884-2895): stores `n7+1..n8` are all 61 stores of the happy path (39 chip RAM, 5 doorbells, 2 Command, 1 cfg 0x80, 14 cfg 0x84), each failed (refused, not applied) and dropped (accepted, not applied). [obs] I repeated it with the real `sw3_wr()` in front of the fake and added a flipped bit: 61 x 3 runs, Command 0x0002 and glue `bm` 0 at the end of each, 0 glue refusals (G3).
- What the sweep cannot see, and what I ran:
  - a failed read: [obs] Z10, each of the 1795 reads of run F failed once: 1795 stops, bus master off in all.
  - the link leaving L0: [obs] Z8c, link down at each of the 61 stores, with config reads still answering and with all-ones: the bit is left set in 34 of 122 cases, each reported as `bus_master=ON (restore not done)` or `bus_master=unknown ...`, never as off; no doorbell after the drop. (One more case is the drop at the last store, the bus-master-off store itself: it went through, the run ends normally with the bit clear.)
  - a stop inside the restore: `swl_stop()` latches and does not re-enter (wr.h:1015-1022); a failed restore store or read-back gives `restore=failed` and `ON (restore not done)` (wr.h:977-987, 1007) [src].
  - a second fault during the restore is outside the single-fault model; the report stays truthful (above).
- Residual state "link down, bit left set, firmware knows the pool": acceptable. No request can cross a down link; the firmware knows only pool addresses (subject to F1); the pool is never freed (glue:407-408, `KMA_PERMANENT`). A link that goes down resets the endpoint's configuration, Command included (PCIe base spec, DL_Down; [guess] for this chip), so the line `ON` overstates rather than understates.
- [obs] `bm_stuck` (Command does not take the bit): stop READBACK, restore writes 0x0002 (test:2856-2857).

### R3 Bounded: PASS

- Loops of run F: ring_info read 15 (wr.h:2065); region check 7 x 7 (wr.h:2086-2097); guard fill/check 256 KiB (wr.h:1993-2015); W20 5 rings; poll `SWM_POLLS` = 200 x 10 ms per request (wr.h:2246), inside it at most 64 items per poll (wr.h:2258); copy `min(rl, omax)` (wr.h:2305); version filter 96 (wr.h:2340).
- Worst-case time added by level 8: 2 requests x 200 x 10 ms = 4.0 s of waiting, plus per poll 16 RC-local reads and 1 BAR2 read, plus prints. A firmware that moves the index on every poll is stopped by the glue's 96-store budget after about 0.6 s ([obs] G4). Upper bound about 5 s after level 7 [obs on the fake clock: Z6a 6.28 s total with a 2 x 1.99 s late firmware, level 7 alone 2.30 s].
- [obs] Z4 (write index jumps to 63): 62 items ignored in one lap, then timeout, bus master off, fake clock 4.3 s.

### R4 Chip-RAM stores: PASS; the TCM-index claim holds on the host side

- Addresses: `shared` from W16, ring_info pointer from the W17 dump, `ringmem` and four index arrays from the 15 words read in W18; each region must be word aligned and inside RAM (wr.h:2018-2021, 2061, 2087) and the seven regions must not overlap (wr.h:2091-2095). `swl_bp_wr()` bounds every word again (wr.h:2030-2033).
- Values: `swl_f_kind()` (wr.h:730-795) accepts host_cap only as 0x1407, host_cap2 only as 0, the two buffers and five rings only with the layout values, a firmware-owned index only as 0, a host-owned index only below the ring depth. An index slot is stored only when its old upper half is 0 (wr.h:1104).
- [obs] Z1a-Z1g: ringmem on host_cap, ring_info inside the shared area, index arrays on host_cap, on a ring base, on each other, on the last RAM word: all stop at W18 with no store. Z2a-c: counts 0xFFFF and 1024: stop, no store.
- 32-bit store into a 4-byte slot: with 2BYTE_INDICES clear the slot spacing is 4 in all three hosts (brcmf pcie.c:1269 `idx_offset = sizeof(u32)`; bwfm.c idx_offset; dhd_pcie.c:13782, 14173-14187) and all three write 16 bits (brcmf `write_tcm16` pcie.c:1270; dhd_pcie.c:6520-6528) [src]. A little-endian 32-bit store of a value below 65536 writes the same low half and a zero upper half. Whether the firmware ignores the upper half is not readable from host source; the old-value rule keeps the patch from changing it. Verdict: correct as far as sources go, U3 stays [guess].
- How the host signals TCM indices: by not writing the host index addresses (see F1). brcmf pcie.c:1254-1272 and dhd_pcie.c:13932-13962 are cited correctly [obs, opened both]. Leaving the four fields untouched is correct only if they are 0: F1.
- ringmem word +4 is stored as `depth | item << 16`; bwfm writes the same bytes as two 16-bit stores (bwfm.c:1410-1413); word +0 of each entry is never written (test:2815) [src].

### R5 Doorbells: PASS

- Only 0x140 and 0x144, value 1, window `SWW_WIN_PCIE`, write-only (`SWT_WO`: no old-value read, no read-back; wr.h:340-341, 1095, 1121-1125), only with `SWF_BM` (and W23 for 0x140). The glue repeats offset, value, live window and master bit (glue:271, 287, 291).
- Register choice for core rev 72 without SHARED_DAR: the three hosts disagree, and the one written for this firmware agrees with the patch. DHD: DB0 is `PCIH2D_MailBox` 0x140 unless DAR is active, and DAR is active only when the firmware sets `PCIE_SHARED_DAR` (dhd_pcie.c:659-666, 13835; dhd_pcie.h:82; pcie_core.h:1030); DB1 is always 0x144, "For DB1, don't use DAR register" (dhd_pcie.c:676-680; pcie_core.h:1031). bwfm: by the SHARED_DAR flag (bwfm.c:1437-1446, 2448-2459); its `sc_pcireg64` (rev >= 64, bwfm.c:1902) selects only the interrupt mask/status registers (bwfm.c:2354-2441), never the doorbell. brcmf: rev >= 64 uses 0xA20/0xA24 unconditionally (pcie.c:452-457, 2475-2478); that table serves other firmwares. With flags 0x70050107 bit 31 is clear, so 0x140/0x144 [src]. U5 is still settled only by the read index moving on hardware.
- The BAR0 window is the PCIe core when they are written: `swl_win(SWW_WIN_PCIE)` at wr.h:2326, nothing in run F writes cfg 0x80 afterwards, and both the header (wr.h:1092) and the glue (glue:291) compare the live cfg 0x80 before each doorbell [src]. That 0x18001000 is the PCIe core is the EROM result the earlier levels use (wr.h:51).

### R6 Firmware-written data is untrusted: PASS (F3 is a function gap)

- Indices: masked to 16 bits, stop when not below the depth (wr.h:2170-2174, 2251-2256). Item pointer from `f_r0 < 64` and constants. Completion: transaction id, response-buffer id, command and "only one" compared (wr.h:2263), length stopped above 8192 (wr.h:2269), status non-zero stops (wr.h:2300). Copy bounded by `omax` (6 or 96, both below the destination sizes, wr.h:2305, 2317). Version text: first line, printable ASCII only (wr.h:2340-2344). MAC: bytes 4 and 5 only (wr.h:2355). Nothing the firmware wrote is used as an address.
- [obs] Z3 (completion with the request's id 0xFFFE): stop DMA. Z5a/Z5b (length 0 / 3): stop MSG. Z12 (two completions): stop DMA. Z6a (answer after 1.99 s): pass; Z6b (2.01 s): stop TMO, and the late firmware does nothing because the bit is clear. All end with Command 0x0002. ASan/UBSan clean.

### R7 No S2MPU/SysMMU/inbound-iATU/interrupt-mask/NVRAM write: PASS

- All stage-3 stores go through `sw3_wr()` (audit `test_store_path_audit_is_clean_on_the_patch`, [obs] in the mutant run). Its offsets: cfg 0x04/0x80/0x84, ATU region 2 (outbound), BAR0 IOCTL/RESET_CTL/bank index/0x140/0x144, BAR2 (glue:262-274). No S2MPU, no inbound ATU (0x100-based offsets are outside `SWW_R2`), no MAILBOXMASK (0x48, 0xC34), no NVRAM path in the patch [src]. The S2MPU block is only read (glue:455-460).

### R8 Levels <= 7 unchanged: PASS

- [obs] my probe: host test and header at `a370142` with the glue chain up to 0154, against HEAD with 0159, the real `sw3_wr()` in front of the fake bus, levels 3-7, bluejay and oriole identity. Output (every non-bulk store, a hash and count of the bulk stores, return code, flags, tuple mask, the captured log): identical, 1330 lines, 0 glue refusals. Store counts 37 / 441045 / 441085 / 441086 / 441097 (bluejay), 37 / 441063 / 441103 / 441104 / 441115 (oriole), the same numbers as review 0154.
- Why: the run F tuples carry run 6 and `swl_wr_ok()` searches runs <= 5 (wr.h:826-829); `swl_wr_pick()` uses that bound below level 8 (wr.h:842). The new take-over term (wr.h:848) can change which tuple reports a refused BAR2 store after W15 at level 7, never whether one is made: the header makes none and the glue latch refuses all (glue:279). `flags` is 16 bits wide and no value changed. The new precondition term is taken only with `SWF_BM`, which only `SWT_BM_ON` sets (wr.h:909, 1146). `swl_stage3()` enters run F at level >= 8 only (wr.h:2421).
- [obs] test:2796: level 7 with the msgbuf fake: no run F store, no sync call, no W18 line.

### R9 Guards: PASS for the pass path (F4)

- Filled in W18 before anything is stored (wr.h:2098), every gap including the tail from 0x22000; checked after a whole-pool sync at the end (wr.h:2346-2351); a changed byte stops with DMA and the restore clears the bit ([obs] test:2875-2876). Not checked on any other stop (F4). A device write outside the pool is not detectable by guards at all.

### 10. Glue: PASS (F9 is the scope note)

- Three gates, all behind `f = l3 >= 8 && released` (glue:261): Command only 0x2, or 0x6 with a pool (glue:269); doorbells only 1, live PCIe window, `h->bm` (glue:271, 287, 291); chip RAM after the release at most 96 (glue:279, 311-313). `h->bm` follows every Command store (glue:308-310).
- Review 0154 F1: a CA7 `RESET_CTL` with bit 0 clear reads the live IOCTL (the live window was compared at glue:291) and, when CPUHALT is clear, needs level >= 7 and the reset vector, then latches (glue:296-304). F2: no store to SYS_MEM or its wrapper after the release (glue:283). Both closed [src]; [obs] the glue sweep and its mutants pass in the mutant run.
- Header can, glue should refuse but does not: any chip-RAM word and value within the budget (F9); cfg 0x04 := 0x2 at level 8 after the release without bus master ever on (harmless); cfg 0x80 any value (older than this patch). Glue refuses what the header needs: nothing ([obs] G1: full level 8 through the real `sw3_wr()`, 0 refusals, `bm` 0, 39 stores counted). [obs] G2: glue without a pool refuses the 0x6 store; the run stops at W22, no doorbell.
- A second stage-3 call in one boot cannot happen in the kernel (`started`, glue:478-481). [obs] Z9 on the header: a second call, same or fresh context, stops at W1 with no store, no doorbell, Command 0x0002.

### 11. The pool and the cache callback: PASS

- `kmem_alloc_contig(kernel_map, &pva, 0x40000, PAGE_MASK, 0, 0, KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT)`, `kvtophys`, contiguity per 16 KiB page, pool not handed over on any failure (glue:407-422): the same call and flags as patch 0104 lines 204-211 and 710-714, which run on hardware [src; the 0104 runs are the project's observation, not mine].
- `flush_dcache64(pa, len, TRUE)` is real on this build: `SWEET.h` defines neither `APPLE_ARM64_ARCH_FAMILY` nor `__ARM_COHERENT_IO__`, so `flush_dcache()` (osfmk/arm/caches.c:61-92) runs `FlushPoC_DcacheRegion` = `dc civac` per 64-byte line (osfmk/arm64/caches_asm.s:421-439) [src]. [obs] in the built kernel: `_flush_dcache64` branches to `_flush_dcache`, and `_FlushPoC_DcacheRegion` is the `dmb sy; dc civac, x0; add x0, #0x40; ... dsb sy` loop (`llvm-objdump -d`, 0xfffffe00074b2644). `TRUE` = physical; `phystokv_range` is valid for kernel RAM. Length is `unsigned`, 0x40000 fits.
- Clean + invalidate is right in both directions provided the host never holds a dirty line over bytes the chip writes. That holds: the response buffer is zeroed and flushed before it is posted (wr.h:2216-2217); the completion ring, scratch and ring-update are written by the host only in the W18 fill, which is followed by a whole-pool flush (wr.h:2098-2099); guard bytes that share a line with the 8-byte scratch are never written again [src].
- Every needed point: ring item before the index store (wr.h:2188); request buffer (wr.h:2234); each completion item before it is read (wr.h:2259); response buffer before the copy (wr.h:2304); completion ring before the timeout diagnosis (wr.h:2291); whole pool before the guard check (wr.h:2346). None missing [src]. No test can tell a missing flush from a present one except by the call count (16, test:2822); see item 14.
- Safe at this point of boot: called from `sweet_ufs_init()` (bsd/dev/sweet_ufs.c:708), thread context; patches 0104/0105 call the same primitive there and later.

### 12. Device-to-host addressing: residual risk stated

What is known: no `dma-ranges`, `use-sysmmu = "false"`, `use-cache-coherency = "false"`, `use-secure-atu = "false"` in the stock tree ([obs], I read the property files); inbound iATU regions 0-3 disabled and S2MPU HSI2 CTRL0 = 0 (PORT.md row o2-w4-mmu1, the project's observation); DHD uses plain bus addresses up to 36 bits on this SoC (F2, [src]). What is guessed: an unmatched upstream request passes the RC unchanged; a disabled S2MPU does not block; the root port forwards (F7).

| If the guess is wrong | What happens | Worse than a hang or panic? |
|---|---|---|
| Requests dropped (root port, RC filter) | No completion, `is untouched`, stop TMO, bit cleared | No |
| S2MPU blocks | Same on the host side; possibly a fault interrupt nobody handles, or the firmware traps | No ([guess] that the unhandled fault line has no effect at EL1; F5 would show it) |
| Address truncated to 36 bits | No effect: the pool, like all RAM of this phone (banks up to 0xA_0000_0000, board.json:143), is below 2^36 | No |
| Address truncated to 32 bits | Contradicted by stock practice: DHD runs with a 36-bit mask, RAM above 4 GiB and no DMA32 zone (F2, note s.2) [src]. If it happened anyway: a pool at 0x8_8xxx_xxxx-0x8_Fxxx_xxxx maps to low DRAM outside XNU's RAM; a pool at 0x9_0xxx_xxxx-0x9_7xxx_xxxx maps to 0x0xxx_xxxx-0x7xxx_xxxx, the SoC's peripheral space | Possibly (a device write into a peripheral register); I rate it very unlikely for the reason given. The `host pool ... at pa` line (glue:418) shows which case a run was in |
| Translated to another address by an inbound region | No enabled region exists [obs W4]; no component between the chip and RAM translates (S2MPU only permits or denies) | Would be: see below |
| Firmware uses an address the host did not give (F1, firmware bug) | A device write into XNU memory outside the pool | Yes |

The last two rows are the real risk for XNU memory: some bytes (four 24-byte items, two answers of 6 and about 100 bytes, possibly index words) written into kernel memory during the roughly 0.1-4 s the bit is set. Run F executes inside `sweet_ufs_init()`, before the root is mounted, and the root is then mounted read-write (tools/xrun:3 "always --rw"). Corrupted kernel memory can therefore lead to a panic (plan stop H10 counts it) or, undetected, to wrong data written to `userdata` later. The UFS driver's partition bounds live in the same kernel memory, so "writes stay inside userdata" is not a guarantee after a stray device write; hitting those few words with a few hundred stray bytes is very unlikely but I cannot call it impossible. No guard can detect a write outside the pool.
I consider this acceptable for the user's own development phone, on three conditions: F1 and F2 are fixed (they remove the two address sources not under the patch's control); the user accepts that `userdata` may need re-flashing after a level-8 run that panics or behaves oddly; a level-8 run that panics outside Wi-Fi code is followed by a fresh `fastboot flash userdata` before the image is trusted again. What the first run should use: `sweetpcie1mmu=1` with F5, the extended `--fail-on` (F6), and no other new feature in the same boot. If one boot without `ufsrw` can deliver its log before `mount(8)` panics, that would remove the disk risk for the first run entirely; I did not verify that it can [guess].

### 13. Message formats: PASS

- Control submit item 40 bytes, control complete 24 (bwfm.c:671, 681; brcmf msgbuf.h:17, 19). Types 0x09/0x0A/0x0B/0x0C (bwfm.h:201-204; brcmf msgbuf.c:38-41).
- IOCTLRESP_BUF_POST: type +0, request id +4, `host_buf_len` +8, address low/high +16/+20 (bwfm.h:263-269; wr.h:2219-2223). IOCTLPTR_REQ: cmd +8, trans +12, input len +14, output len +16, address +24/+28 (bwfm.h:229-238; wr.h:2236-2243). Completion: status +8, resp_len +12, trans +14, cmd +16 (bwfm.h:271-274, 284-291; wr.h:2263-2268). The completion's request id is the posted buffer's id (bwfm.c:2557-2558; brcmf msgbuf.c:866), which is what the patch compares with `SWM_RESP_ID` [src].
- `WLC_GET_VAR` 262 (bwfmreg.h:310). Request length 20 for `cur_etheraddr` = name + NUL + 6; 256 for `ver` is the author's [guess], harmless (zero-filled, inside the 2048-byte request region). Timeout 2000 ms (bwfm.h:188; brcmf msgbuf.c:28).
- Ring depths and sizes for version 7: 64x40, 1024x32, 64x24, 1024x24, 1024x40 (bwfm.c:671-691; brcmf msgbuf.h:10-23). ring_info word indices and the u16 counts at 0x34/0x36/0x38 (bwfm.h:168-185, bwfm.c:601-604).
- host_cap for 0x70050107: 7 | 0x400 (HOSTRDY_DB1 set) | 0x1000, no DAR bit = 0x1407 (bwfm.c:542-558). DHD reaches the same two capability bits on its "no device wake" branch (dhd_msgbuf.c:3845-3868) and adds many more; whether the three are enough for this firmware is U7 [guess].
- Host-ready before any buffer is posted, as bwfm (bwfm.c:732-734). bwfm also opens the interrupt mask first; the patch does not (U4, polled).

### 14. Test-first not kept: the tests are adequate for the store path, not for the protocol

- For the question "can a store go wrong or bus master stay on", the host test, the fault sweep, the glue sweep with the real `sw3_wr()` text and the mutants do make up for the order. I could not find a store-path hole with 15 scenarios of my own.
- The fake firmware is the author's reading of the same sources as the code. It cannot catch: whether the real firmware reads TCM indices when DMA_INDEX is set; whether it uses ring_info host addresses (F1); the D2H sync race (F3; the fake writes the item before the index); whether 0x140/0x144 reach this firmware; whether host_cap 0x1407 is enough; whether completions come with the interrupt mask closed; anything about the RC, S2MPU or address width (F2, F7); cache behaviour (`f_sync` only counts calls, test:2732-2739, and no mutant removes one); reads on a dead link (the fake keeps answering; I added all-ones in Z8b/Z8c); the glue's pool allocation, which is text-audited only (mut:172-183).
- EQUIVALENT claims, one by one (seven entries in the run F block): `short pool accepted` (mut:329): holds for safety (a shorter pool fails the bound; only a longer pool is accepted). `sync callback not required` (mut:334): not equivalent, killed by a NULL call. `in-RAM check ignores the RAM base` (mut:340): holds (unsigned wrap gives at least 0xFFE00000). `bus master tuple needs no W21` (mut:341): does not hold for a direct call ([obs] Z14). `bus master on from any old Command` (mut:342): holds in a deterministic fake (`swl_wpre()` checks the same word first). `host-ready doorbell needs no bus master` (mut:351): holds for the step functions; a state that tells them apart (bit on, off again, then the first host-ready) is not produced, and the glue refuses it. `short MAC answer accepted` (mut:363): does not hold ([obs] Z5b). See F8.

### 15. Worst case on the phone, and what the run command must fail on

- Worst case: a device write into kernel memory outside the pool (item 12): panic, or silent damage that reaches `userdata` after the root is mounted. Recovery: power cycle, re-flash `userdata`. No boot partition is written by anything in this patch; the chip gets no persistent change (no OTP, no NVRAM write).
- Next: hang or panic while the bit is set (bus error on a TCM read after the link dropped, as at level 7): power cycle.
- Expected failure modes: `no completion` (DMA blocked, firmware silent, F3 race, wrong doorbell); firmware trap.
- The run must fail on: `restore=failed`, `DMA suspect`, `bus_master=ON`, `bus_master=unknown`, `restore of group 16 skipped`, any panic or kernel trap pattern; and must not count as PASS without `stage 3 level 8 done msgbuf round trip ok, bus master off`. After F1: also on the W18 refusal line for non-zero host index addresses.

## Commands and results

All through `pixos-run.sh`; heavy ones under `flock ~/pixelios-artifacts/scratch/heavy.lock`.

| Command | Result [obs] |
|---|---|
| `cd shim && PIXELIOS_HOST_TESTS=always ./build.sh --board oriole --avb --pstore-fast --usb-gadget --out out/rev0159` | rc 0; `PASS wlan_wr_host_test (compile)`, `RESULT: PASS wlan_wr_host_test`, `PASS wlan_wr_host_test`; 302 PASS lines, no failing check; images built |
| `gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror -Iout/rev0159 -o /tmp/claude-1000/t0159 wlan_wr_host_test.c && /tmp/claude-1000/t0159` | `RESULT: PASS wlan_wr_host_test`, rc 0, no sanitizer report |
| `python3 -B tools/test_wlan_wr_mutants.py` | full run by me (45 min): `Ran 9 tests in 2696.814s`, `OK`, rc 0 (header mutants, store-path audit, board glue, rev gate, glue sweep, glue mutants). The author's run in `~/pixelios-artifacts/scratch/w8-mutants.log` shows the same (`Ran 9 tests in 2412.359s`, `OK`) |
| `python3 -B tools/test_build_wlan_level.py` | `Ran 4 tests ... OK` |
| `tools/patch-numbers check` | `RESULT: PASS this tree's patch numbers do not overlap other branches or reservations` |
| `grep -a 'SWEET WLAN: W2\|fw version\|host pool' ~/pixelios-artifacts/xnu-local-w8-3337859/kernel.development.sweet` | the run F strings are in the kernel: `host pool %u KiB at pa %#llx %s`, `W18 no usable host pool`, `W18 ok: host pool`, `W20 %u rings set up in the host pool`, `W22 bus master ON`, `W24 device-to-host DMA works`, `W25 DMA suspect`, `W25 bus master off, guards intact`, `fw version %s mac xx:xx:xx:xx:%02x:%02x` (kernel sha256 starts `0c702dfce0ccbfbc`) |
| `cmp` of `shim/out/rev0159/sweet_wlan_wr.h` and `sweet_wlan_chip.h` with the patched tree | identical |
| Probe R8 (levels 3-7 before/after, real `sw3_wr()`, both boards) | outputs identical, 1330 lines, 0 refusals |
| Probe G (level 8 through the real `sw3_wr()`) | G1 rc 0, 0 refusals, 39 stores, `bm` 0; G2 no pool in the glue: stop at W22, no doorbell; G3 61 stores x {failed, dropped, bit flipped}: 0 bad; G4 budget: refused at 96, stop, `bus_master=off` |
| Scenarios Z (scratch copy of the host test with extra fake knobs, ASan/UBSan build, 8 min) | below |

My scenarios (the scratch files are not in the tree; the fake knobs are small edits of `fw_cpl`, `fw_msgbuf`, `f_rd`, `f_wr`, `f_delay`, `fw_place`):

| id | scenario | result [obs] |
|---|---|---|
| Z1a-g | ring_info pointers on host_cap, inside the shared area, on a ring base, on each other, on the last RAM word | stop at W18 (SHARED or MSG), no run F store, Command untouched |
| Z2a-c | max_submissionrings 0xFFFF, 1024; max_completionrings 0xFFFF | stop at W18, no store |
| Z3 | completion carries the request id 0xFFFE instead of the response buffer id | stop DMA at W24, `bus_master=off` |
| Z4 | write index jumps to 63 | 62 items ignored, stop TMO, `bus_master=off`, bounded |
| Z5a/b | response length 0 / 3 with status 0 | stop MSG, `bus_master=off` (Z5b kills the mutant of mut:363) |
| Z6a/b | answer after 1.99 s / 2.01 s | pass / stop TMO, late firmware sees the bit clear and writes nothing |
| Z7 | Command reads 0x0006 before W22 (set at each of the 43 earlier run F stores, and from boot) | stop PRE each time, no Command store, no doorbell; the bit stays, no `bus_master` line (F6) |
| Z8a/b | link drops exactly at the bus-master store; config reads answer / read all-ones | stop PRE at W23, `bus_master=ON (restore not done)` / stop READBACK at W22, `bus_master=unknown (store not read back, restore not done)`; no doorbell |
| Z8c | link drops at each run F store, both read models | 34 of 122 leave the bit, all reported, none as off |
| Z9a/b | second stage-3 call, same and fresh context | stop STATE at W1, 0 stores |
| Z10 | each of the 1795 run F reads fails once | 1795 stops, bit clear in all |
| Z11 | each of the 61 run F stores with one bit flipped | bit clear in all, no out-of-pool address followed |
| Z12 | two completions for one request | stop DMA, `bus_master=off` |
| Z13 | write index moves before the item bytes land | `completion item type 0 ignored`, stop TMO, `bus_master=off` (F3) |
| Z14 | bus-master tuple called directly at level 8 before W21 | `-SWW_E_PHASE`, no store (F8) |
| Z15 | ring_info host index addresses non-zero | run passes, nothing printed (F1) |

## What I could not verify

- Anything about the real firmware: that it reads TCM index slots when DMA_INDEX is advertised and the host addresses are 0; that those addresses are 0; that it ignores the upper half of an index slot; that host_cap 0x1407 is enough; that it completes requests with the interrupt mask closed (U3, U4, U7, F1).
- Device-to-host addressing on the phone: pass-through with no inbound region, S2MPU with CTRL0 = 0, the root port's Command state (U1, U2, F7). The W4 values are the project's observation, not mine.
- That `kmem_alloc_contig` with these flags returns a usable pool on the phone (same call as patch 0104, not run by me); for `flush_dcache64` I read the source and disassembled the built kernel, nothing more.
- The PCI-spec statements (Status write-1-to-clear, DL_Down resets the endpoint, root-port Bus Master Enable) are from memory; the spec is not among the pinned sources.
- Whether a boot without `ufsrw` gets its log out before `mount(8)` panics (item 12).
- xrun's behaviour with an explicit `--fail-on` (F6) was read, not run.
