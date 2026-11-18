# 56: oriole on the upstream release 2026-11-15 (Step 1: merge, host only)

Branch `wip/oriole-upstream` (kernel) = `wip/oriole-next` (33af822, audio daily image 4) + `origin/main` f084ce5
("PixOS release 2026-11-15", bluejay Wi-Fi/BT/GPU, patches up to 0368). Userland: `pixelios-userland`
`wip/oriole-upstream` = `wip/catalog-contiguous` (836bdc6) + `origin/main` d5b2079. Plan: `~/.claude/plans/bubbly-pondering-rabbit.md`.
All claims below are *read from source* / host results unless marked otherwise; nothing here ran on the phone.

## Patch renumber map (old -> new)

Our 0145-0182 collided with upstream's 0145-0182 (different content). Kept patches were rebased onto upstream 0001-0368
(in order, on xnu-8796.141.3) and renumbered with `tools/patch-numbers next` (owner `oriole-upstream-port`).

| old | new | slug | rebase notes |
|---|---|---|---|
| 0145 | 0369 | sweet-touch-linux-post-reset | boot-arg parse hunk re-seated (upstream 0355/0356 irq block) |
| 0146 | 0370 | sweet-touch-rstfirst | 3 hunks re-seated on upstream's tg_run (irq stage 0/1 loop): rstfirst pulse, wait, irq-low gating at the loop top |
| 0152 | 0371 | sweet-usb-log-port | upstream rewrote the log thread; `sweetusblogport` also drives the hand-built panic frame's UDP port (new) |
| 0156 | 0372 | sweet-trusty-probe | conf/files + sweet_ufs.c init list (after `sweet_bt_init`) |
| 0158 | 0373 | sweet-amp-init | conf/files + init list (`sweet_amp_init` stays before the touch thread: shared gpp23 pad words) |
| 0161 | 0374 | aoc-pre | `sweet_usblog_sent++` moved into upstream's `ud_log_drain` (after a sent chunk) |
| 0162 | 0375 | trusty-tipc | conf/files (fuzz only) |
| 0164 | 0376 | aoc-load | conf/files (fuzz only) |
| 0165-0172 | 0377-0384 | aoc-start, aoc-audio, aoc-playback, aoc-reply-match, amp-dsp-load, amp-dsp-chunk, amp-dsp-mpu, amp-tone | clean |
| 0173 | 0385 | aoc-sweetaudio-dev | `sweet_pre_reset()` (amp power-down hook) added before the remote resets (upstream now does a clean stop first) and before the thermal-CRIT reset (upstream 0336) |
| 0174-0180 | 0386-0392 | amp-diag, amp-diag-cspl, amp-hal-fmt, amp-pcm-detect, amp-slot-scan, amp-vi-tx, aoc-probe | clean; NOT squashed (no rebase work to save), boot-arg levels unchanged |
| 0181 | 0393 | sweetaudio-ep1 | `sweet_pad_spin` extern re-seated in the touch includes |
| 0182 | 0394 | sweetaudio-polish | clean |

Number references inside the kept patches and in our tools, host tests, audio/ sources, board.json and doc 55 were mapped
old -> new. References to a dropped patch read `old-oriole-NNNN`; the dev-overlay zlib allocator reference (old 0163) points
at upstream 0154 (`sweet-devoverlay-zlib`, same pattern). PORT.md, docs/research 46/50/52-54 and docs/reviews keep the old
numbers (history). Boot-arg names are unchanged (`sweetaoc`, `sweetamp`, `sweetaudiotest`, `sweetusblogport`,
`sweettouchrstfirst`, `sweettrusty`, ...); only the patch numbers moved.

## Dropped (superseded by upstream)

| old | slug | replaced by |
|---|---|---|
| 0147, 0149, 0150, 0154, 0159, 0160 | wlan fw-ident, pcie1-mmu-probe, fw-ready, ca7-latch, msgbuf, rc-busmaster | upstream Wi-Fi stack (0121-0200, plain contiguous DMA 0167/0174) |
| 0148 | sweet-hsi2c-fg (fuel gauge) | upstream `sweet_chg_i2c` (0283/0284/0298/0307) + `sweet_thermal` (0336) |
| 0155, 0157 | mali replay, mali irq | upstream Mali IRQ (0272) and GPU stack |
| 0163 | dev-overlay zlib | upstream 0154 / 0280 / 0317 |

Their host tests and mutant scripts went with them (`shim/hsi2c_fg_xnu_host_test.c`, `mali_replay_host_test.c`,
`sweet_spi_host_test.c`, `tools/test_hsi2c_fg_audit.py`, `test_mali_replay_mutants.py`, `test_sweet_spi_mutants.py`,
`test_build_wlan_level.py`, `pack_gpujob.py` + test, `test_sweetgl_once.py`); our `wlan_wr_host_test.c` /
`test_wlan_wr_mutants.py` / devoverlay tests were replaced by upstream's. `gpu/` is removed (GPU user space is upstream
`pixelios-userland` `tools/mesa-patches`, `sweetgl-*`; our tree stays on `wip/oriole-next`). `/sweet-batt` (mkadt) is kept as
board data; its consumer (old 0148) is gone.

## Parked (TODO)

- **batt-publish (old 0153), Step 2.** It published the old 0148 reader's samples into SweetBattery. Upstream 0336 already
  feeds IOPMPowerSource Temperature from `sweet_thermal`; the percentage stays the static fallback. Re-seat = record the
  RepSOC/VCell/AvgCurrent of upstream's thermal/MON reads (registers already on its allowlist) and expose them like
  `sweet_batt_sample()`; do it after Step 2 shows that upstream's charger/thermal path is safe on oriole (gpp23 pins 4/5 are
  shared with the amps: put upstream's pad accesses under `sweet_pad_spin` or confirm they do not write).
- **Oriole Wi-Fi firmware identity (old 0147), Step 3.** Values are known (*observed*, run wlan-oriole-l4, old 0147 header):
  oriole vendor `fw_bcmdhd.bin` = **1764091 bytes, CRC32 0x92567396** (sha256 8a52ffe1...); bluejay = 1764018 / 0x1153ccff.
  Upstream still pins one identity (`SWW_FW_LEN/CRC` in `sweet_wlan_wr.h`, checked at sweet_wlan.c:1077 and
  sweet_wlan_wr.h:1026/1624) and bounds the image with `SWW_RAM_MIN 1791226` (so `SWW_IMG_MAX` = 1764018 < oriole's image:
  W8 refuses it). The patch needs: a {model, len, crc} table selected by the ADT root `model-number` (= board since this merge),
  `SWW_RAM_MIN` 1791299 (oriole RAM observed 2752512 B, run wlan-oriole-l3), and the pinned literals of upstream's
  `shim/wlan_wr_host_test.c` / `tools/test_wlan_wr_mutants.py` (441005 words, 0x3AEAB0/B4, `SWW_FW_LEN == SWW_IMG_MAX`)
  updated. Not written in Step 1: it reworks upstream's pinned test/mutant suite and is only checkable with Step 3's firmware.

## Plumbing re-applied on upstream files

`tools/boards.py --target` (BOARD) in xrun, build-xnu-image.sh, fboot.sh, fflash-userdata.sh (userdata_mib of the target),
probe-run, phone-state (+ oriole self-test rows), safety-check (S8 product = target; guard needles follow fboot.sh),
to-fastboot.sh text; tools/phone (auto-merged). mkpayload.py `--aocfw/--ampfw`, `xnu_bootargs_default`, `provisional`,
`--board-cmdline`; mkadt.py model-number = board, product-description per board, /sweet-batt. xrun: on a board with
`xnu_bootargs_default` (oriole) the bluejay TOUCH DEFAULTS are not added (board.json's recipe wins), `--aocfw/--ampfw`,
`SWEET_USBLOG_PORT` (listener port + `sweetusblogport=`), and the oriole early stop without the UDP reset when the image has
`--restart S,bootloader`. shim.c `bluejay_*` functions are gs101-generic (same SYSIP/SWRESET registers) and already run on
oriole; not renamed.

## What Step 2 must know (upstream xrun changes, *read from source*)

- Every xrun needs a run plan `docs/runplans/<label>.md` (a `## Questions` table + every `--expect` regex verbatim) or
  `XRUN_NO_RUNPLAN="reason >= 10 chars"` on the command line.
- Recovery: `--restart S` (or `sweetrestart=S` in --args) is required, or a `no-recovery:` line in the run plan.
- `--userdata-check`: the newest line of `$A/flashed-userdata.txt` must be an iOS root flash, else
  `XRUN_USERDATA_BYPASS="reason"`.
- `tools/safety-check pre/fastboot/monitor` run inside xrun (battery voltage >= 3500 mV, storage temp < 38 C, S1 battery
  temp >= 45 C stop, silent-boot stop); S8 now checks the BOARD target.
- Default `debug=0x8` (was 0xc); `debug=` with 0x4 needs `--panic-spin "reason"`. Default `cpus=8`. Boot line limit 2048
  (upstream 0320); the kernel must carry patches 0176 + 0320 (it does).
- The dev overlay is always built with the STANDARD ROOT copies (`tools/overlay_std.py`, `$A/std-extras`).
- `tools/fflash-userdata.sh` now needs `--reason`, a formal `GATE PASS` on the clean head and `--not-overlayable` for a
  non-standard root.
- Upstream mkadt adds `/sweet-chg` (soc.json chg_probe, gs101: applies to oriole), `/bluetooth` and `/product/audio`
  (D22AP copy, `PIXOS_ADT_AUDIO=0` = off) by default; check them on the first oriole boot.
- **Charger write by default.** Upstream xrun adds `sweetchgi2c=1 sweetchgicl=900` to every boot (patches 0284/0287: the
  charger input-current-limit WRITE, CNFG_09), and `safety-check pre` S3 refuses a boot line without them (they start the
  rows that carry the battery temperature); `--no-chg` therefore fails S3. On oriole this is an unverified charger write:
  Step 2 must check upstream's charger path against oriole's DT (and the shared gpp23 pads) before the first boot, or ask.
- `sweetbatt=1` (old 0148) no longer exists: run-preflight refuses it as an unknown boot-arg; drop it from the daily args.
- Upstream xrun also adds `sweetwdt=60`, `sweetremote=1` and `sweetepoch=<now>` (see the `implicit:` line).

## Host results (2026-11-18)

- All 344 patches apply in order (xnu-8796.141.3 + stagers, scratch tree `$A/scratch/up-xnu`); kernel built locally
  (`XNU_BUILD_WHERE=local`, `HEAVY_JOB_HELD=1`: upstream's heavy-job gate wants 12 GB free, this host has ~3 GB with the
  browser open): `$A/kernels/kernel-up-4adf69c7.sweet`, sha256 4adf69c7a3a3425c865952fee5622c6393ea5103a1aad2841f2b53a9896a33c0,
  make exit 0, 0 error lines. Note: upstream xnu-local-build pruned the cached work trees c4, o2-local1, w7, gpu, audio, w8
  (caches only; the kernels in `$A/kernels` are untouched).
- `PIXELIOS_HOST_TESTS=always shim/build.sh --board oriole --avb --pstore-fast --usb-gadget --out out/up`: PASS; `--board bluejay` builds too (incl. the
  oriole audio host tests and amp mutants).

## Expected Step 2 command (current audio daily config on the merged kernel)

    SWEET_USBLOG_PORT=64738 XRUN_NO_RUNPLAN="step 2 regression boot of the upstream merge" \
      tools/xrun --board oriole --kernel ~/pixelios-artifacts/kernels/kernel-up-4adf69c7.sweet --cpus 8 --lock-wait 600 \
      --restart 330,bootloader --gui --aocfw ~/pixelios-artifacts/aoc/aocf.bin --ampfw ~/pixelios-artifacts/amp/ampfw.bin \
      --label up-reg1 --args 'sweetaoc=15 sweetenv=mediaserverd:DYLD_INSERT_LIBRARIES=/usr/local/lib/libsalog.dylib,SALOG_TAP_NULL=1'

(+ `XRUN_USERDATA_BYPASS="..."` if `$A/flashed-userdata.txt` does not end in an iOS root flash line; the charger defaults
above apply unless Step 2 decides otherwise.)

## Step 2 results (2026-11-18, observed; rows in device/google/oriole/docs/PORT.md)

- boot_b = boot_b-orig.img again (2869bc43…). Charger path matches bluejay on oriole's live DT (HSI2C12 0x10D50000, slaves
  0x25/0x36/0x66/0x69, gpp23-4/5 func 2 pull 3); upstream's charger code only reads gpp23. Upstream's charger args stay on;
  on a USB 2 (480 Mb/s) port P3 refuses the CNFG_09 write (stays 500 mA, net discharge ~0.1-0.4 A).
- batt-publish re-seated as **0395** (`sweet-batt-gauge-publish`): SweetBattery publishes FG RepSOC (84-88 % observed).
- Merge regression fixed: xrun again adds `sweetusblogport=$SWEET_USBLOG_PORT` (a66b72a).
- Passing kernel `$A/kernels/kernel-up-7089e31f.sweet` (run up-reg3). One unexplained stop in up-reg2 (log ends ~+155 s,
  phone off USB 19 min, back in fastboot at 45 C storage temperature); not reproduced.
