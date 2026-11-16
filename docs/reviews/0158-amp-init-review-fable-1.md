VERDICT: SAFE-AFTER-FIXES

# Review: xnu patch 0158 sweet-amp-init (`sweetamp=1`), independent safety review

Reviewer: Claude Fable 5.2 (delegate, worked alone, nothing run on the phone, patch not changed). Author trailer of the commit: Claude Opus 5.5. Date 2026-11-16.
Commit reviewed: `808cf81` on `wip/oriole-audio`, in a clean detached checkout (`/tmp/claude-1000/review-0158`, removed afterwards).

Line refs:
- `h:N` = line N of `bsd/dev/sweet_amp.h` as extracted from the patch (= line N + 176 of `xnu/patches/0158-sweet-amp-init.patch`).
- `c:N` = line N of `bsd/dev/sweet_amp.c` (= patch line N + 25).
- `test:N` = `shim/amp_xnu_host_test.c`.
- AMP = `~/.hermes/cache/scratch/src-gm-amplifiers/cs35l41/` (git 33976d7).
- GKI = `ios-pixel6/pixel6-oriole-kernel/drivers/base/regmap/` (git a4662a84).
- XT = the patched tree `~/pixelios-artifacts/localbuild/work/audio/xnu/`. Its `bsd/dev/sweet_amp.h` and `sweet_amp.c` are byte-identical to the patch content (`cmp`) **[obs]**.
- K = `~/pixelios-artifacts/xnu-local-audio-808cf81/kernel.development.sweet`, sha256 `a1b68f69d292fa5d9769c4495e7103afcbd48d500b50806cb6c686dc87224c00`.

Tags: **[obs]** = I ran it or read it in a built file; **[src]** = read from source at the cited line; **[guess]** = not shown by anything I could open.

## Summary

The patch cannot write `PWR_CTRL1` (0x2014) and cannot produce amplifier output. The 12 (register, value) pairs equal the stock B2 probe writes, in stock order. The pad / controller / reset handling is the shim code at pk-amp `4c7be85` (review fixes F1-F3 included), unchanged.

Two things must be fixed before the phone run:
- **F1**: an amp whose `OTP_BOOT_ERR` bit is set still gets the 12 writes, and the run then prints the pass line `SWEET AMP: init ok ... errors=0`.
- **F2**: the host test does not pin the identification gate, the read-back compare, one error bit and the chip-select check of the write path (5 of my 20 mutants survive). The patch must be regenerated for F1, so these tests are needed first.

One thing must be fixed before the pass line is used in the ladder: **F3** (no OTP unpack, so plan step A7 is only partly done).

Everything else is optional.

## Findings

| id | sev | finding | required fix |
|---|---|---|---|
| F1 | M | The write gate is `el == AMP_OK && l.otp && l.devid == AMP_DEVID && l.revid == AMP_REVID` (`h:967`, same for R at `h:973`). `OTP_BOOT_ERR` (IRQ1_STATUS3 bit 31) is not in it. Stock fails the probe on that bit before any write (AMP `cs35l41.c:4244-4249`), and plan stop H8 names "OTP error". The pass line is printed when `il == AMP_OK && ir == AMP_OK && restored` (`h:984-985`), which also ignores the bit, while the return value (`h:988-990`) and the RESULT line (`h:766`) do check it; the glue discards the return value (`c:144`). Scenario X5c / X5d **[obs]**: L (or both) with `OTP_BOOT_ERR`: 12 writes to each such amp, lines `SWEET AMP RESULT: l=none r=none restore=ok` and `SWEET AMP: init ok l,r devid 35a40 rev b2, errors=0` in the same run, rc=1. The existing case `test:649-650` passes because it checks neither the write count nor the pass line. No output risk (the same 12 writes), but it breaks "no write to an amp that was not identified" and gives a false pass. | Add `!(st3 & AMP_OTP_BOOT_ERR)` to both gates. Print the pass line from the same condition as the return value. Host test: `OTP_BOOT_ERR` on L, on R, on both gives `nwr == 0` for that amp and no `init ok` line. **Before the phone run.** |
| F2 | M | Host test gaps. Of 20 mutants of the header that I ran against the committed test, these survive **[obs]**: gate without `l.otp` (m10); gate without `r.revid` (m13); `BST_DCM_UVP_ERR` removed from `AMP_ERR_BITS` (m08); read-backs of IRQ1_MASK1 / CCM not compared (m12); the "other chip select reads high" check removed from `amp_write` (m09). (m02 and m14 also survive but are redundant checks: the allowlist still refuses.) There is no mutant script and no source / object audit for the XNU header; the only thing that pins these lines is this review of this exact text. | Add cases that fail for m08, m09, m10, m12, m13: wrong REVID, wrong DEVID, OTP never done, each on L and on R, with `nwr == 0`; each of the 6 error bits on each amp; a read-back that differs; a write while the other chip select reads low. **Before the phone run** (the F1 fix regenerates the patch). A mutant script like the shim's is recommended, see item 10. |
| F3 | M (ladder) | `cs35l41_otp_unpack` is not done (`h:14-15`), but plan step A7 lists it and the pass line is the plan's A7 line. A later step could read "A7 PASS" as "the amps are trimmed". | Record the result as A7 partial (no OTP unpack) wherever the line is used; any step that sets `GLOBAL_EN` must require the OTP unpack first. **Before the line gates A9 / A10.** No hardware effect in this patch, see "OTP unpack". |
| F4 | L | After a failure on L (error bit, read-back mismatch, not identified) the run still releases R and writes its 12 registers (`h:967-974`). X6a **[obs]**: L reports `BST_SHORT_ERR`, R still gets 12 writes. H8 only forbids output steps, so this is not a rule violation, but "stop on the first amp error" is the more conservative behaviour, and L stays out of reset about 0.15 s longer. | Optional: skip the R writes (or all of R) when `il != AMP_OK` because of an error bit. |
| F5 | L | The TX allowlist checks each byte against the armed frame at its FIFO position, and TX-on checks only the FIFO level and the arm state (`h:432-434`, `h:449-452`). It does not know which bytes are queued. A caller that changes the arm state inside one frame gets a mixed frame accepted. X9 **[obs]**: 6 armed header bytes of `(0x40, 0x5555)`, disarm, TX-on unarmed with level 6 accepted, then a 4-byte RX transfer: the fake amp sees a write frame to 0x40 with the value clocked from an idle MOSI. X10 **[obs]**: over all arm states the per-position sets can spell address 0x00002018 (`PWR_CTRL2`) but not 0x00002014 (no position accepts byte 3 = 0x14). No code path does this: `amp_arm` is set only in `amp_write` (`h:653-655`) for one `xfer`, and stale bytes after a failed `xfer` make every later frame fail on its last byte (argued under item 2; the per-position fault loop `test:666-673` shows no violation **[obs]**). So "0x2014 is unreachable" holds for the allowlist alone; "only whole allowlisted frames" holds for the code flow, not for the allowlist alone. | Optional hardening: call `flush()` on the early error returns of `xfer` (`h:597`, `h:600`, `h:603`, `h:605`), and let `w32` keep a shadow of the bytes queued since the last SW_RST and accept TX-on only if the shadow equals one whole allowed frame. |
| F6 | L | `AMP_ERR_BITS` (`h:182`) is the six named bits of AMP `cs35l41.h:684-689`. `CS35L41_INT1_MASK_DEFAULT` 0x7FFCFE3F unmasks bits 31, 17, 16, 8, 7, 6; bit 16 has no name in the pinned header and bit 15 (`TEMP_WARN` 0x8000) stays masked. X6c **[obs]**: bit 16 set gives the pass line. IRQ1_STATUS1 is also read only after the writes, never before. | Optional: treat `0x800301C0 | 0x8000` as error bits; read and print IRQ1_STATUS1 once before the first write. |
| F7 | L | What `BSTCVRT_DCM_CTRL` = 0x51 (reset default 0x2001, AMP `cs35l41-tables.c:59`), 0x4100, 0x4310 and 0x4400 do bit by bit is not in the pinned sources (no field names in `cs35l41.h`, 0x4310 has no name at all). That they cannot start the boost converter while `GLOBAL_EN` = 0 is datasheet knowledge **[guess]**. The pinned evidence: stock writes exactly these values at every boot, on this phone, before `GLOBAL_EN` is ever set (`cs35l41.c:4357-4359`, `GLOBAL_EN` only at `:2292-2294`) **[src]**, and both amps probe (REF dmesg per the round-1 review) **[obs there]**. | None. Add the datasheet to the pinned sources if it can be had. |
| F8 | L | Text errors. "exactly those 13 (register, value) pairs" twice (`h:16`, `h:189`); there are 12 (`h:185-187`). The IRQ mask cite `cs35l41.c:4300-4302` (`h:9`, patch line 4) is `:4306-4308` in the pinned copy. The glue says the run "takes about 0.3 s" (`c:7`); the worst case is about 9.3 s (item 8) and is stated nowhere. The read set of `tx_byte_ok` (`h:383-388`) is per position: 2 x 3 x 3 x 5 = 90 addresses are readable, not 7 (all start with 0x80 / 0x82 = reads; none is an OTP_MEM word, the only precious registers, AMP `cs35l41-tables.c:610-618`). | Correct the texts. |
| F9 | L | Writing IRQ1_MASK1 unmasks six interrupt sources; the amp's INT pin can then pull gpa6-3 / gpa9-6. The patch never writes those pads (read-only windows, `c:33-34`, `c:48-50`) and XNU has no handler on them **[src for the patch, guess for "no other handler"]**. The amp is back in reset about 0.15 s later. | None. |

## Per-item verdicts

### 1. Write allowlist = the 12 stock B2 probe writes: PASS

The list is one macro, `AMP_INIT_SEQ` (`h:185-187`); `amp_write_ok` (`h:190-200`) and the write loop (`h:675-676`) both expand it, so the allowlist and the sequence cannot differ. `PWR_CTRL1` is refused by name first (`h:194-195`) and is not in the list.
X3e **[obs]**: `amp_write_ok` over 76,230 pairs (10 registers incl. 0x2014 x 7 values, every single-bit flip of the register and of the value) agrees with an independently typed 12-pair table.

Each pair against the pinned source **[src]**, in stock order:

| # | register | value | stock source | what it does | can it enable output / boost / clock? |
|---|---|---|---|---|---|
| 1 | IRQ1_MASK1 0x10110 (`cs35l41.h:132`) | 0x7FFCFE3F (`cs35l41.h:702`) | `cs35l41.c:4306-4308` | Unmasks the critical-error interrupts (bits 31, 17, 16, 8, 7, 6) | No. Interrupt routing only (F9). |
| 2 | TEST_KEY_CTL 0x40 (`cs35l41.h:30`) | 0x5555 | `cs35l41.c:3676` | Test-key unlock 1/2 | No. Opens the test registers for writes 4-9. |
| 3 | TEST_KEY_CTL 0x40 | 0xAAAA | `:3677` | Test-key unlock 2/2 | No. |
| 4 | VIMON_SPKMON_RESYNC 0x4100 (`cs35l41.h:80`) | 0 | `:3678` | Voltage / current monitor resync setting | Not established from the pinned sources (F7); monitor path, not the output stage **[guess]**. |
| 5 | 0x4310 (no name) | 0 | `:3679` | Unnamed test register | Not established (F7). |
| 6 | VPVBST_FS_SEL 0x4400 (`cs35l41.h:84`) | 0 (default 1, `cs35l41-tables.c:65`) | `:3680` | Sample-rate select of the VP / VBST monitor | Not established (F7); monitor ADC **[guess]**. |
| 7 | BSTCVRT_DCM_CTRL 0x381C (`cs35l41.h:76`) | 0x51 (default 0x2001, `cs35l41-tables.c:59`) | `:3681` | Boost converter discontinuous-mode control | Configuration of the boost converter, not its enable. The enable is `BST_EN` in `PWR_CTRL2` (`cs35l41.h:669-671`, control at `cs35l41.c:1799`) plus `GLOBAL_EN`; neither is written. That the converter does not switch with `GLOBAL_EN` = 0 is **[guess]** (F7). |
| 8 | DSP1_YM_ACCEL_PL0_PRI 0x2BC20E0 (`cs35l41.h:330`) | 0 | `:3682` | DSP memory-accelerator priority | No. The DSP core is not running. |
| 9 | DSP1_XM_ACCEL_PL0_PRI 0x2BC2020 (`cs35l41.h:324`) | 0 | `:3683` | same, X memory | No. |
| 10 | TEST_KEY_CTL 0x40 | 0xCCCC | `:3684` | Test-key lock 1/2 | No. |
| 11 | TEST_KEY_CTL 0x40 | 0x3333 | `:3685` | Test-key lock 2/2 | No. |
| 12 | DSP1_CCM_CORE_CTRL 0x2BC1000 (`cs35l41.h:318`) | 0 | `cs35l41.c:4401` | DSP core control: 0 = core not enabled, reset bit 0x200 clear (`cs35l41.h:734`) | No. It keeps the DSP stopped. |

- The errata table is applied with `regmap_multi_reg_write` at `cs35l41.c:4357-4359` under `case CS35L41_REVID_B2` (`:4351`), table at `:3675-3686` **[src]**.
- None of the 12 is `PWR_CTRL1` 0x2014, `PWR_CTRL2` 0x2018, `PWR_CTRL3` 0x201C, `PLL_CLK_CTRL` 0x2C04, `DSP_CLK_CTRL` 0x2C08, `SP_ENABLES` 0x4800, `AMP_GAIN_CTRL` 0x6C04 (`cs35l41.h:42-44`, `:53-54`, `:85`, `:120`) **[src]**. So no clock, PLL, serial port or gain setting changes.
- None is an OTP control or OTP memory register (0x400-0x51C, `cs35l41.h:32-41`). The unlocked test key is used for writes 4-9 only and is locked again. Nothing in the amp can be programmed permanently by these writes **[src for the addresses; guess that no other test register side effect exists]**.
- Together: the amp's power state after the 12 writes is its reset state (`PWR_CTRL1` default 0, `cs35l41-tables.c:30`), which the round-1 ruling R0 accepted for the reset release. Stock sets `GLOBAL_EN` only in the DAPM power-up event (`cs35l41.c:2292-2294`).
- Stock makes more probe writes than these 12 (`cs35l41_irq_gpio_config` at `:4276`, the OTP unpack at `:4385`, `cs35l41_dsp_init` after `:4407`). The patch leaves them out; it adds none.

### 2. Frame discipline: PASS (note F5)

Write framing against the sources **[src]**:
- `reg_bits 32, val_bits 32, pad_bits 16`, both big endian (AMP `cs35l41-spi.c:32-45`).
- regmap-spi sets only `read_flag_mask = 0x80` (GKI `regmap-spi.c:107`), no write flag; `_regmap_raw_write_impl` formats the register and applies `write_flag_mask` = 0 (`regmap.c:1772-1774`), so bit 7 of byte 0 is clear in a write.
- The value is formatted behind register + pad (`regmap.c:1971-1972`); the pad bytes are 0 because the work buffer is `kzalloc`'d (`regmap.c:1135`).
- One bus write of `reg_bytes + pad_bytes + val_len` = 10 bytes (`regmap.c:1856`; `regmap_spi_write` = one `spi_write`, `regmap-spi.c:28-33`). `regmap_multi_reg_write` without `can_multi_write` writes the pairs one by one in order.
- The patch: `wframe_byte` (`h:203-207`) = address with bit 31 cleared, 2 zero bytes, value, big endian; `_Static_assert` on the sizes (`h:215`). The fake amp decodes frames with its own typed layout (`test:212-233`); mutant m16 (little-endian value) is caught **[obs]**.

Arming **[src]**, **[obs]** for the X cases:
- `amp_arm` is written only in `amp_write` (`h:653-655`, cleared `h:658-660`), after `amp_write_ok` (`h:646`), for exactly one `xfer(..., 2, ...)`.
- Armed, a TX byte is accepted only if the pair is allowed, the transmitter is off, the FIFO level is below 10 and the byte equals the frame byte at that level (`h:450-451`). TX-on needs the level to be exactly 10 (`h:432-433`). X4: level 9 and 11 refused, `PACKET_CNT` 9 / 11 refused, an eleventh byte refused. X4c: a second armed frame queued behind a full one is refused.
- Not armed, the rules are the shim's: byte 0 is 0x80 or 0x82, TX-on only at level 6 (`h:434`, `h:452`). X4b: armed TX-on at level 6 refused, unarmed TX-on at level 10 refused, an armed 0x80 at position 0 refused. So a read frame cannot become a write and a write frame cannot become a read, as long as the arm state does not change inside a frame (F5).
- A stale arm cannot be used: X1 (arm left on for an allowed pair before the run) gives 0 frames, exact restore; X2 (armed with 0x2014 = 1) the same; X2b-X2e, X3-X3d: `amp_write` of 0x2014 (either value), 0x80002014, `PWR_CTRL2`, a value one bit off, an address one bit off, 0x381C = 0x2051, TEST_KEY = 0x55 all return an error with 0 MMIO writes and 0 frames.
- Stale bytes after a failed `xfer` (it returns without `flush()` at `h:603`): the next frame's bytes are checked at position = stale count + index, so the last byte of an armed frame meets level >= 10 and the first byte of a read frame (0x80 / 0x82) meets a position >= 1 where no set contains it. No TX-on follows. In XNU `try_write32` cannot fail on a mapped word (`c:70-81`), so this needs a FIFO level that reads wrong.

### 3. Identified amps only, never both chip selects low, CS40L25 untouched: FINDING F1, otherwise PASS

- Gate: `h:967` / `h:973`: transfer status ok, `OTP_BOOT_DONE` seen, DEVID == 0x35A40, REVID == 0xB2 (`cs35l41.h:727`, `:732`). **`OTP_BOOT_ERR` is missing (F1).**
- X5a (L REVID 0xB0), X5b (R DEVID 0x35B40), X5e (L never `OTP_BOOT_DONE`): 0 writes to that amp, run fails, exact restore **[obs]**. Each amp is gated on its own identification; the other amp is still written (F4).
- Chip selects: `amp_write` refuses unless the other chip select reads high (`h:648-649`), as `amp_read` does (`h:626-627`); the own chip select is raised on every exit (`h:661`). X8: write to R while CS0 reads low: 0 MMIO writes, 0 frames **[obs]**. The fake bus flags "both chip selects low" (`test:269-270`); none in any run. This check in `amp_write` is not pinned by a test (F2, m09).
- CS40L25: no I2C block and no gpp24 word is mapped or named. The mapped windows are the seven of `c:27-35`; the writable words are the 21 of `h:391-396`, all SPI13 / sysreg USI13 / CMU mux / gpp25, gpp1, gpp16, gpp23 **[src]**.

### 4. After the writes: PASS (notes F4, F6)

- IRQ1_STATUS1 (0x10010, `cs35l41.h:124`) is read after the 12 writes (`h:679`). Checked bits (`h:182`): `AMP_SHORT_ERR` 0x80000000, `TEMP_ERR` 0x20000, `TEMP_WARN` 0x8000, `BST_SHORT_ERR` 0x100, `BST_DCM_UVP_ERR` 0x80, `BST_OVP_ERR` 0x40 = `cs35l41.h:684-689`, the same six the stock handler acts on (`cs35l41.c:2058-2141`) **[src]**. X6d: each of the six on each amp fails the run, prints the `!!! amp error bit` line, exact restore (12/12) **[obs]**.
- Read-backs: IRQ1_MASK1 must equal 0x7FFCFE3F and DSP1_CCM_CORE_CTRL must be 0 (`h:681-683`, `h:697`). Whether the real chip returns reserved mask bits as written is a **[guess]**; a mismatch gives `init FAILED`, never a pass.
- Failure inside an amp's init stops that amp's writes at once (`h:675`: each write only if the previous returned ok) and the run goes on to the restore; it does not stop the other amp (F4).
- Restore on every path after the first MMIO write: there is no `return` between `h:943` and `restore()` at `h:978` **[src]**. The restore first drives both reset bits to 0 by read-modify-write (`h:830-831`) and requires both to read 0 (`h:867`).
- The register writes do not survive the run: the reset line clears the amp's registers. The stock "AMP reset" path marks the whole register cache dirty before the pulse, "invalidate all cached values which have now been reset" (`cs35l41.c:508-510`), and restores the registers afterwards (`cs35l41_restore`, `:549`) **[src]**. Chip behaviour itself is **[guess]** (no datasheet). If the restore cannot put a reset low, the run says so (`!!! RESTORE FAILED`, `restore=FAILED`) and the amp stays out of reset with the 12 writes and the output stage off.

### 5. Shim review findings F1-F3: PASS

The part of the header from `#define AMP_BASE` on differs from pk-amp `shim/ampid.c` at `4c7be85` (the tip of that branch, the F1-F3 fix commit) only by the A7 additions (`diff -w`) **[obs]**. The header was not generated from older shim code.
- F1: any reset DAT bit that reads 1 refuses before any write (`h:815-817`); host cases `test:634-635` **[obs]**.
- F2: restore by `pad_put` of the reset bits := 0 and the chip-select bits := snapshot (`h:830-831`, `h:836-837`); only amp fields compared, both resets must read 0 (`h:864-867`); host cases H7 / H7b / H8 (`test:651-658`) **[obs]**.
- F3: the RESULT line prints `none` unless read, OTP done and no OTP error (`h:766-767`). The new pass line does not follow that rule: finding F1 of this review.
- The round-2 review of the shim code is still open; a finding there applies here too.

### 6. MMIO: PASS

- The only store is `try_write32` (`c:70-81`), called only from `w32` (`h:492`). `w32` checks `amp_reg_writable` first (`h:484`), reads the word (`h:486`; for TX_DATA / PENDING_CLR it reads STATUS instead), then `w32_allowed` (`h:490`) **[src]**.
- The writable set is the shim's: `amp_reg_writable` is unchanged; `w32_allowed` differs only by the armed branches for CH_CFG, PACKET_CNT and TX_DATA (`h:432-433`, `h:448`, `h:450-451`) **[obs: diff]**. Sysreg: mode field only; CMU: the mux select bit only; pads: the amp pins' CON nibbles, DAT bits and the two DRV nibbles of gpp25 (`h:419-422`, `h:454-475`).
- ALIVE / FAR_ALIVE pad pages are mapped with `ro = 1` and `sa_va(pa, 1)` returns NULL for them (`c:33-34`, `c:48-50`); they are also not in `amp_reg_writable`.
- No PUD word, no IRQ pad, no CMU divider / gate / QCH word is writable; unit cases `test:538-549` **[obs]**.
- `ml_io_map` of the sysreg page 0x10C21000 (not 16 KiB aligned): `io_map` keeps the in-page offset (XT `osfmk/arm/io_map.c:95-132`), so the word at +0x14 is the right one **[src]**.
- Not checked at object level: there is no object audit of the kernel (see "could not verify").

### 7. Gate and ordering: PASS

- Without `sweetamp=1` nothing is mapped or read (`c:121-123`); other values refuse (`c:124-127`); the root `model-number` must be exactly "oriole" with its NUL (`c:128-133`); `tools/mkadt.py:488` writes the board name there **[src]**. One run per boot (`c:117-120`).
- `sweet_amp_init()` is called in `sweet_ufs_init` after `sweet_keys_gs101_start()` and before `sweet_touch_gs101_init()`, `sweet_wlan_init()`, `sweet_batt_init()` (XT `bsd/dev/sweet_ufs.c:708-713`) **[src]**. It creates no thread and returns only after the restore.
- Threads already running then: the log, sample and keys threads. The keys thread only reads pad DAT words (XT `sweet_keys_gs101.c:74`, `:86`). No other file under XT `bsd/dev` names a PERIC0 / PERIC1 pad address **[obs: grep]**; drivers that take addresses from the ADT (usb, gpu) were not traced **[guess that none is a PERIC pad]**.
- Afterwards: CON of gpp23 / gpp16 / gpp1 / gpp25 and DRV of gpp25 are written back from the snapshot; the DAT bits of other pins are never changed on purpose. A DAT read-modify-write copies the input level of the other pins of the bank into their output latches (round-1 F7), which has no effect while those pins are inputs or functions. Host: `restored_exact()` in every passing and stopped run **[obs]**. On oriole the touch driver does not start (no `/sweet-touch`), and the battery thread maps its pad bank only later.

### 8. Bounded waits: PASS (note F8)

- Loops: `counter_runs` 1000 reads; `wait_ms` n ms or 1,000,000 x n reads; `wait_lvl` 10 ms or 100,000 polls; `mux_idle` 1 ms or 100,000 polls; RX drain 128 reads; 100 OTP polls; 12 writes; one DEVID retry (`h:506-559`, `h:576`, `h:720`, `h:675-676`, `h:732-733`).
- Worst case with every transfer ending just inside its limit: 20 ms per transfer (10 ms wait + 10 ms flush wait); a read is 2 transfers, a write 1. Per amp: 4 ms reset + 100 x (1 + 40) ms + 4 x 40 ms + 12 x 20 ms + 3 x 40 ms = 4.62 s. Both amps about **9.3 s**. Expected on a healthy bus: well under 0.3 s.
- The run takes no lock, does not sleep and allocates nothing; it polls `mach_absolute_time()` in the thread that calls `sweet_ufs_init` (`sweet_ufs_rootdev`, XT `sweet_ufs.c:1010`). A preemption only lengthens a wait. That nothing at that point of boot objects to a 9 s busy thread is a **[guess]**.
- Not bounded by software: an MMIO read that takes an SError. XNU has no guarded read (`c:9-10`); the shim run A1 must have passed first.

### 9. Worst case on the phone: PASS

- A panic or hang on an MMIO access, cleared by a power cycle.
- An amp left out of reset with its default registers plus the 12 writes, reported by `!!! RESTORE FAILED`. Output stage off: `PWR_CTRL1` cannot be addressed (X10: no arm state accepts byte 3 = 0x14), its default is 0, and no audio clock is started by this patch.
- Every word written is volatile SoC or amp register state. No OTP, PMIC, storage or secure-world access.

### 10. Host test quality: FINDING F2

- The test does cover: the 12 writes in order on both amps, `PWR_CTRL1` never written, the armed allowlist unit cases, refusals with zero writes (15 cases), a faulting write at every write position, dropped writes, loud restore failure, the F1 / F2 cases of the shim review.
- 20 mutants **[obs]**: 12 caught (among them `PWR_CTRL1` added to the sequence, a wrong value, swapped order, L written without identification, arm never cleared, armed byte not checked against `amp_write_ok`, unarmed TX-on with 10 bytes, little-endian value, restore not driving RST_L low, pass line always printed, error bits never checked). 5 real survivors, see F2. 2 redundant-line survivors. 1 control (the F1 fix itself stays green, which shows the test does not notice the gate either way).
- Is a mutant script required before the run? Not for the hardware safety of this exact text: the properties that protect the phone (allowlist, single writer, restore) were read line by line and exercised here. But the patch must change for F1, and then only tests pin it. Required: the cases listed in F2. Recommended: commit a mutant list (the 20 above are a start) so that the build fails when a gate line is lost.

## OTP unpack not implemented

- In the pinned source the errata patch runs **before** `cs35l41_otp_unpack` (`cs35l41.c:4357-4359`, then `:4385`), not after it **[src]**. So up to and including write 11 the patch issues the same writes stock has issued at that point.
- None of the registers of the 12 writes is a target of the OTP unpack. The register addresses in the two OTP maps (`cs35l41-tables.c:726-933`) are 0x2030, 0x208C, 0x2090, 0x300C, 0x394C-0x395C, 0x4000, 0x400C, 0x410C, 0x4160, 0x416C, 0x4170, 0x4360, 0x4448, 0x444C, 0x6E30-0x6E64, 0x7068, 0x7418, 0x741C, 0x7434, 0x17040, 0x17044 **[obs: enumerated]**. So skipping the unpack does not change what the 12 writes leave in their registers.
- The unpack uses a different key sequence (0x55 / 0xAA, 0xCC / 0x33, `cs35l41.c:1915`, `:1921`, `:1970`, `:1976`); the allowlist refuses those values (X3d) **[obs]**.
- Verdict: skipping the unpack makes none of the 12 writes unsafe. They are not meaningless: they prove the SPI write path, the test-key sequence and two read-backs. What the run does not prove is a trimmed amp. An untrimmed amp must never get `GLOBAL_EN` (F3).

## Hostile scenarios (scratch copy of the host test, the header from the patch, ASan + UBSan; not committed)

| # | scenario | result **[obs]** |
|---|---|---|
| X1 | arm left on for an allowed pair before the run | 0 frames, 0 amp writes, `init FAILED`, exact restore |
| X2 | armed with (0x2014, 1) before the run; `amp_write` of 0x2014 / 0x80002014 / `PWR_CTRL2` | 0 frames; each `amp_write` returns an error with 0 MMIO writes |
| X3 | value one bit off, address one bit off, 0x381C = 0x2051, TEST_KEY = 0x55; 76,230-pair sweep of `amp_write_ok` | all refused with 0 MMIO writes; sweep agrees with the 12-pair table |
| X4 | frames of 9 and 11 bytes, `PACKET_CNT` 9 / 11, read-length frame while armed, 10 bytes unarmed, a second frame behind the first | all refused |
| X5a / b / e | L REVID 0xB0; R DEVID 0x35B40; L never `OTP_BOOT_DONE` | 0 writes to that amp; run fails; the other amp still written |
| X5c / d | `OTP_BOOT_ERR` on L / on both | **12 writes to each such amp and the pass line** (F1) |
| X6a / b | error bits on L, on both | flagged, `init FAILED`; R still written after L's error (F4) |
| X6c | IRQ1_STATUS1 bit 16 on L | pass line (F6) |
| X6d | each of the 6 error bits on each amp | 12/12 fail, flagged, exact restore |
| X8 | write to R while CS0 reads low | 0 MMIO writes, 0 frames |
| X9 | `w32`-level mix: armed header, disarm, unarmed TX-on, RX transfer | accepted by the allowlist; the fake amp sees a write to 0x40 (F5); not a code path |
| X10 | union of accepted bytes per position over all arm states | 0x2014 not composable; 0x2018 composable (F5) |

In every full run: no violation of the fake bus rules (no write outside the sequence, never both chip selects low, no write to an amp in reset), `restore=ok` only with an exact restore.

## Commands run and results

All through `pixos-run.sh`; the shim build under `flock ~/pixelios-artifacts/scratch/heavy.lock`. All **[obs]**.

1. Clean checkout: `git -C pk-audio worktree add --detach /tmp/claude-1000/review-0158 808cf81`; removed at the end.
2. `cd shim && PIXELIOS_HOST_TESTS=always ./build.sh --board oriole --avb --pstore-fast --usb-gadget --out out/rev0158` → exit 0, 308 lines starting with `PASS`, no line starting with `FAIL`. The header the build extracted (`out/rev0158/sweet_amp.h`) is byte-identical to the one I reviewed. Its amp lines:
   - `PASS amp_xnu_host_test (compile)`
   - `ok: allowlist unit cases`
   - `ok: armed write unit cases`
   - `ok: full run, mux on the TOP source (switched to oscclk and back) (1005 writes, 2354 reads, 42 frames)`
   - `ok: dropped writes from any position never pass`
   - `ok: a dropped restore write is loud (!!! RESTORE FAILED, restore=FAILED)`
   - `PASS amp_xnu_host_test` (34 `ok:` lines)
   - `PASS amp_xnu_asan_host_test (compile)`
   - `PASS amp_xnu_asan_host_test`
3. `tools/patch-numbers check` → `RESULT: PASS this tree's patch numbers do not overlap other branches or reservations`, exit 0.
4. `grep -a 'SWEET AMP' K` (as `grep -a -o` and `strings`) → present in the built kernel:
   - `SWEET AMP: refused sweetamp=%u (only 1 is defined), not touched`
   - `SWEET AMP: refused not an oriole ADT (model-number), not touched`
   - `SWEET AMP: refused ml_io_map(`
   - `SWEET AMP: init run (sweetamp=1), timebase %llu Hz`
   - `AMP: init ok l,r devid 35a40 rev b2, errors=0` (once)
   - ` !!! amp error bit in IRQ1_STATUS1 (H8: no output steps for the rest of the run)`, `!!! RESTORE FAILED: `, `sweetamp`
5. `cmp` of the patch's two new files against XT `bsd/dev/sweet_amp.h` / `sweet_amp.c` → identical.
6. `diff -w` of the header (from `#define AMP_BASE`) against `git -C pk-amp show 4c7be85:shim/ampid.c` → only the A7 additions (item 5).
7. Hostile copy of the host test (`-fsanitize=address,undefined`, the 34 original cases first, then X1-X10) → exit 0, no sanitizer report; results in the table above.
8. 20 mutants of the header, each compiled with the build's flags against the committed `shim/amp_xnu_host_test.c` → 12 caught, 5 real survivors (m08, m09, m10, m12, m13), 2 redundant-line survivors (m02, m14), 1 control (item 10).
9. Enumeration of the `reg` values in the OTP maps and `grep` for the registers of the 12 writes in them → no hit ("OTP unpack").

Not run: any phone command; a kernel build.

## What I could not verify

- Anything on hardware.
- The compiled kernel beyond its strings: no disassembly, no object audit of `w32` / `w32_allowed` in K. `__attribute__((noipa))` is a GCC attribute; what clang did with it is not checked (it does not matter for the allowlist logic, only for an audit that wants one symbol).
- CS35L41 behaviour that needs the datasheet: bit meanings of the errata registers (F7), boost and output stage idle with `GLOBAL_EN` = 0, registers cleared by the reset pin, a truncated write frame not being committed, reserved bits of IRQ1_MASK1 on read-back, the meaning of IRQ1_STATUS1 bit 16.
- That no XNU code outside `bsd/dev/sweet_amp.*` writes the four pad banks during the run, for drivers that take their addresses from the ADT.
- That a busy thread of up to 9.3 s at that point of boot has no side effect (expected run time is well under 0.3 s).
- The round-2 review of the shim code this header was ported from.
- Whether a Fable review of Opus-written code satisfies AGENTS rule 6.
