# 56: oriole on the upstream release 2026-11-15 (Steps 1-8: merge, Wi-Fi, BT, GPU, daily images 5 and 6, UI fixes)

Branch `wip/oriole-upstream` (kernel) = `wip/oriole-next` (33af822, audio daily image 4) + `origin/main` f084ce5
("PixOS release 2026-11-15", bluejay Wi-Fi/BT/GPU, patches up to 0368). Userland: `pixelios-userland`
`wip/oriole-upstream` = `wip/catalog-contiguous` (836bdc6) + `origin/main` d5b2079. Plan: `~/.claude/plans/bubbly-pondering-rabbit.md`.
Step 1 sections are *read from source* / host results; the Step 2-7 sections are marked observed per run.

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

## Step 5 GPU prep (2026-11-18, host only; nothing ran on the phone)

- **libsweetgl.dylib** = upstream `tools/ci/mesa-panfrost-ios.yml.draft` stages 1, 2, 5 (3/4 are link tests, covered by stage 5's
  checks): Mesa 26.2.5 tarball (sha256 794b3d17…, = the pin) + userland 13a4025 `mesa-patches` 0001-0008, `sweetgl-eagl`, `sweetgl-mali`.
  Output `$A/gpu/up/out/libsweetgl.dylib`, 22567408 B, sha256 `8efaad3e3572c02c6ccecc5a1c0511d58bcbc9db2bd4314a1d8cada7bb85fe4e`; every
  stage-5 check passes (platform iOS minos 16.0, arm64, system deps only, install name /usr/lib, 282 undefined all `(from …)`, the 13 symbols,
  one export `_sweetgl_state`, constructor, ad-hoc signature). Host-setup deviations only (scripts `$A/gpu/up/scripts/`): NixOS + nix
  llvm/clang 21.1.8 instead of macos-14/Xcode; an `llvm-config` wrapper reports a joined llvm+clang libdir (no Mesa patch); theos
  iPhoneOS16.5.sdk + theos ld64 instead of xcrun's SDK; IOKit headers from that SDK (no MacOSX SDK); llvm-ar; ninja -j3; `ldid -S`
  for `codesign -f -s -`; `otool -l` for `vtool`.
- **Standard extra**: `$A/std-extras/manifest.tsv` + `usr/lib/libsweetgl.dylib` (intent gpu). `overlay_std.py prepare --intent gpu`
  gives 2 files (the dylib + the standard launchd.plist), compressed archive 5460352 B. Payload = kernel 16929248 + 5460352 = 22.4 MB,
  inside the 24.3 MB oriole has booted (PORT G5 ramp8); +aocf/ampfw would be 25.7 MB (unproven). After the first applied gpu boot the
  ledger records the extra and later gpu runs do not carry it.
- **Audio vs the std overlay**: the sweetaudio launchd job was only in overlay-written launchd.plists; ALWAYS_RESET has restored the
  root's plist since up-reg1 (up-reg2: `skipped launchd.plist (identical)`). The installed HAL driver, VirtualAudio move, libsalog and
  /usr/local/pixos are not ledger paths and are never touched. Old GPU files (/usr/local/lib/libsweetgl.dylib, gli-slots.txt,
  sweetgl_once/selftest, /private/var/dev/ramp-*) are unused residue: no whiteout needed; optional 0280 cleanup overlay
  `$A/gpu/up/ov-old-gpu-clean` (9 whiteout markers, audio paths excluded).
- **Board deltas** (read from source + oriole PORT rows): sweetgpu 0x1C500000, GPU_ID/shader_present (observed oriole 2026-11-14),
  sweetfvmap 0x206A000 (P0.1 oriole), recores 0x1e / cpus=5 / cpu order (gs101 DT), --dvfs maxima (oriole CPU fvmap rows equal),
  G3D/G3DL2 DVFS pair rates (gs101 table), fb 1080x2400 stride 4320 (oriole board.json, equal), dot pitch 458 (an iPhone-class value,
  not a panel measurement), nav-bench 1080x2400 coordinates: all SoC/iOS-generic. Per chip: the fvmap VOLTAGES of the clock-level
  undervolt check -> board data `gpu_dvfs_uv` (commit 6f2b599; oriole unset until a gpu boot prints `SWEET FVMAP: dom 5|6 lv`).
  Not checked on oriole: MIF fvmap rows (xrun `--mif-clk` top row 3172000), nav taps on this root's home-screen layout.

## Step 3 Wi-Fi / Step 4 Bluetooth (2026-11-18; rows in device/google/oriole/docs/PORT.md)

**Board deltas: none.** Read from oriole's live DT (`$A/ref/oriole/20261114T072643Z/dt`): PCIe1 reg blocks (DBI 0x14800000, ELBI
0x14520000, PMA 0x14550000, PCS 0x14540000, cfg 0x60FFE000), PERST gph2-0, WLAN_EN gpp14-2 and their pinctrl (perst func 1 drv 3;
clkreq gph2-1 func 2 pud 3 drv 3; cfg_wlanen func 1) equal `soc.json` wlan and the pinned `wlan_seq.json` rows; the shim wlan files are
unchanged from wip/oriole-next (which reached fw ready on oriole). BT: shutdown gpp16-2, device-wakeup gpp14-3, host-wakeup gpa6-6,
serial@175B0000 uart16-bus gpa3-0..3 func 2 = the SBU_* constants of upstream 0189. Nothing moved to board.json. *observed (DT capture)*

**Firmware identity (patch 0396, `sweet-wlan-fw-ident-oriole`).** `swl_fwids[]` {model, len, crc}: bluejay 1764018 / 0x1153ccff (row 0,
also any other/missing model), oriole 1764091 / 0x92567396, chosen by the ADT root `model-number`; `swl_wx.pin_len/pin_crc` feed the
W14 checks (0 = stop). `SWW_RAM_MIN` 1791226 -> 1791299 (+73 B): `SWW_IMG_MAX` 1764091, 441023 words, `SWW_IMG_END` 0x3AEAFC. NVRAM
(oriole bcmdhd.cal 24552 B, varsize 21388) and CLM (15976 B = 12 chunks <= 12 x 1400) needed no change. Tests: upstream's
`wlan_wr_host_test` / `test_wlan_wr_mutants.py` stay on the header through 0174 (bluejay pins unchanged); new
`shim/wlan_fwid_host_test.c` (header + the 0396 hunk) covers oriole (row choice, bounds, swl_fw_pin both ways, no bus access).
Container `$A/wlan-fw/wlanfw-oriole.bin` (pack_wlanfw: FW 1764091 crc 0x92567396, NVRM 24552 crc 0x35ffab80, CLM 15976 crc 0x1205970c;
1804688 B, identical to the old pack). BT: oriole's vendor `BCM.hcd` sha256 = 450f8c66… = upstream's pinned stock hash, so
`tools/bcm-hcd-stack --hcd $A/bt-fw/BCM.hcd --out $A/scratch/hcd-stack-up` works unchanged (no second hash needed).

**Kernel** `$A/kernels/kernel-up-820ba87b.sweet` (0001-0396 + local `9000-sweet-bt-hcd-image.patch`), sha256
820ba87b9246d5081a89ca0f9a2dfe175f7c5c00c38410305baf608139a13e90, 17404424 B.

**Host fix on the way:** 6f2b599 (GPU prep) made `GPU_CLK_UVS` per board; oriole has none, so `gpuclk_host_test` level 1/3 failed in every
oriole shim build. 56dce35: those tests include a header copy with the bluejay rows they cite.

**Cooling:** after each Wi-Fi boot the fastboot storage temperature read 44-46 C and cooled slowly in fastboot (36-41 C after 20-30 min);
xrun's S2 gate (< 38 C) held every boot. One pre-boot read of 45 C (wlan-up1b) was followed by 31-32 C reads seconds later.

**Results (observed, kernel 820ba87b; rows wlan-up1c..up6-all, bt-up1):**
- Wi-Fi: fw `RTE (PCIE-MSGBUF) 20.101.90 ... BCM4389 r3`, WLC_GET_VERSION + cap (up1c, sweetwlan=20). sweetwlan=23 stops at the escan
  with BCME -26 (no CLM/country before L22c; up2). sweetwlan=24: CLM 12/12 chunks, country XZ, `scan done bss 17 channels 5`, join link
  up (up3). sweetwlan=26 + sweetwlankeep=1 (DHCP credentials): station MAC tail ..:a5:b1, keys installed, en0 hand-over, `A80211 kept link
  announced on en0`, iOS DHCP lease on en0 (pixping `iface en0 ready addr=192.168.2.x`), gateway ping 10/10 twice, wifid devices=1 (up4).
- BT: patchram build 203 / hci_rev 0x20cb verified, VSC adapter on, LE scan 385 advertising reports in 20 s (rf witness), inquiry 3
  results, stock bluetoothd alive with controller traffic (bt-up1). BT is on by default (0295); every boot of this kernel ran patchram.
- Combined (audio blobs + wlanfw + compressed pixping overlay): payload 22.85 MB (sum of parts), boot line 689 B; both attempts (up5, up6)
  stopped by the kept S1 battery-temperature check (46.1 / 46.8 C) after the Wi-Fi join, so audio + BT were not re-confirmed in that boot.
- **Daily args** (on top of the audio daily args and xrun defaults): `sweetwlan=26 sweetwlankeep=1` + image flags `--wlan-hold --wlanfw
  wlanfw-oriole.bin` + the WLANCRED blob (DHCP: network 2 keys, no IP/GW); BT needs no arg (0295 defaults: sweetbt, sweetbtkeepbaud,
  sweetbtpatchram, sweetbtlefeat) but the kernel must carry the BCM.hcd stack. pixping/sweetnull are test-only.
- Safety: `SAFETY_NO_COOLDOWN=<reason>` (3119ec2, user 2026-11-18) overrides S2 only. Back-to-back boots heated the phone: storage
  44 -> 54 -> 57 C before boots, battery 46+ C at the start of the last two (S1 stop). Remaining: one combined boot with the battery < 45 C.

## Step 5 GPU (2026-11-18; rows gpu-o-up1, gpu-o-up2 in device/google/oriole/docs/PORT.md)

**Result: upstream's GPU stack draws oriole's UI with no oriole-specific code change** (*observed*, kernel `$A/kernels/kernel-up-820ba87b.sweet`,
sha256 820ba87b9246d5081a89ca0f9a2dfe175f7c5c00c38410305baf608139a13e90; libsweetgl.dylib 8efaad3e via the std extra, first applied in gpu-o-up1).
The old oriole failure (black framebuffer, refused `b3a8` IOSurface formats) did not occur: format-0 surfaces are accepted
(`sweetgl: ABGR8 ... swizzle wzyx`); framebuffer geometry from board.json was right as is.

- **gpu-o-up1** (pair 3 = G3D 572 / G3DL2 750 MHz, no MIF request, CPU dvfs read only): `sweetgl: SweetMali user client open` +41.7 s,
  `EAGLContext swizzles installed` +41.8 s, `SWEET FB: display on` +47.6 s, first `SWEET FB: hash changed` +47.8 s, **GPU-drawn lock screen
  (SBCoverSheet) at +54 s** after `fastboot boot`, `SWEET FPS: swaps 36 distinct 36`, touch scanning +81 s. Nav bench ran (`SWEET NAV: done`
  +307 s): gpu-fps-bench v2.1 `fps_min=25.6 (app-launch) fps_pooled=43.6 low1_min=7.5`; fps-wholerun pooled 15.4 (includes idle spans).
  `general-open` / `about-open` never animated (0/3): the fixed Settings > General tap does not match this root's Settings list, so
  gpu-nav-score FAILs on coverage; every other action animated 3/3 (the home-screen taps do match).
- **gpu-o-up2** (upstream defaults: pair 1 = 762 / 996 MHz, MIF 3172000, CPU maxima, cpus 5): GPU path identical (`display on` +44.7 s,
  lock screen +48 s, `SWEET FPS: swaps 61 distinct 61`), then **stopped by S1 at the 55 C ceiling** (55.3 C, ~+220 s) before the nav bench
  finished. The boot started hot (first kernel thermal sample 54.0 C, X1-DROP) because it followed up1 with no cool-down.
- **Oriole fvmap voltages** (sweetfvmap boot, gpu-o-up1): G3D 848000/737500, 762000/700000, 701000/681250, 572000/650000; G3DL2 996000/750000,
  885000/750000, 750000/712500 (uV) = 12.5-37.5 mV below the bluejay unit. Now board data `gpu_dvfs_uv` (05c1b11).
- **Safety**: S1 override `SAFETY_BATT_TEMP_MAX` (7c7f61c, oriole only, (45, 55] C, logged by the monitor; also fixes the
  `fb_storage_ok` mutant needle that 3119ec2 broke). The kernel's own backstop (patch 0336) already parks at 55 C on two samples; X1 drop at 48 C.
  Temperatures: fastboot storage 50-63 C between boots; battery 47.5 -> 49.4 C (up1), 54.0 -> 55.3 C (up2).
- Camera: none on this host, runs use `--no-camera` (no screen frames; the fb hash is the picture witness; picture correctness = the user's look).
- Known upstream defects (components/gpu.md): search magnifier glyph and a dark band in Spotlight; 60 fps not reached on every step
  (slowest 51.6 at boost on bluejay); no Metal; no idle GPU power-down.

**Combined run (GPU + audio + Wi-Fi + BT): NOT RUN** (ladder stopped at the 55 C ceiling, as instructed). Prepared command (the daily
configuration; WLANCRED from a 0600 temp env under `$A/scratch` with `PIXOS_ENV_FILE=<it> PIXOS_WIFI_NET=2`, wifi.env `SSID`/`PSK` mapped to
`PIXOS_WIFI2_SSID`/`_PSK`, `PIXOS_WIFI2_SECURITY=wpa2-psk`, deleted afterwards). A wlan-intent run, because xrun builds the WLANCRED blob only
for `--intent wlan`; the GPU flags are given explicitly and the dylib (already on userdata, ledger extra) is not re-sent:

    SWEET_USBLOG_PORT=64738 SAFETY_NO_COOLDOWN=... SAFETY_BATT_TEMP_MAX=55 XRUN_NO_RUNPLAN=... tools/xrun --board oriole \
      --kernel $A/kernels/kernel-up-820ba87b.sweet --cpus 8 --intent wlan --gpu-handover --gpu-clk-level 1 --mif-clk --gui --no-camera \
      --aocfw $A/aoc/aocf.bin --ampfw $A/amp/ampfw.bin --wlan-hold --wlanfw $A/wlan-fw/wlanfw-oriole.bin \
      --overlay $A/scratch/pixping-ov/tree --compress --restart 640,bootloader --lock-wait 600 --label wlan-gpu-o-all1 \
      --args 'sweetaoc=15 sweetenv=mediaserverd:DYLD_INSERT_LIBRARIES=/usr/local/lib/libsalog.dylib,SALOG_TAP_NULL=1;backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/libsweetgl.dylib,SWEETGL_BOCUT=1,SWEETGL_BOPOOL=1,SWEETGL_FLUSHT=1,SWEETGL_SPLITHEAP=1,SWEETGL_OVERLAP=1 sweetmali=1 sweetmaliirq=1 sweetmaliovl=1 sweetmalipend=3 sweetmalicache=2 sweetmalipoll=20 sweetfps=2 sweetfbhash=1 sweetwlan=26 sweetwlankeep=1 sweetnull=devd,backboardd,SpringBoard,pixping,pixopen,pixnet'

Boot line 978 B (`--print-bootargs`, limit 2048). Payload (sum of parts, as wlan-up5-all): kernel 17404424 + aocf 3296808 + ampfw 41768 +
wlanfw 1804688 + pixping overlay ~280192 (compressed) + ADT 22252 + WLANCRED 128 = ~22.85 MB (no GPU dylib). cpus 8 without the gpu
X1-first cpu order / sweetrecores (gpu intent only) is untested with the GPU. For the daily image drop the test-only parts (pixping overlay,
pixping/pixopen/pixnet in sweetnull, sweetfps, sweetfbhash). Run it with the battery well below 55 C, or the S1 ceiling stops it again.

## Step 7: combined run, daily image 5 (2026-11-19; rows in device/google/oriole/docs/PORT.md)

**Cool-down first** (user 2026-11-18, one time): the phone came out of gpu-o-up2 at 65 C storage / 56.4 C battery. In recovery (battery sysfs
readable, backlight set to 0 in RAM) it reached 39.7 C after ~40 min (23:29 -> 00:08 +04); no further cool-down waits after that.

**Results (observed, kernel 820ba87b, git d044311):**
- **wlan-gpu-o-all1** (the prepared command with `--gpu-clk-level 3`, no `--mif-clk`, `--restart 480,bootloader`): GPU UI (sweetgl open +77 s,
  fb hash changing, lock screen +91 s), touch +98 s, `/dev/sweetaudio up` +84 s, Wi-Fi join +71 s + DHCP +86 s + gateway ping 10/10 twice,
  BT patchram build 203 + bluetoothd, battery published (RepSOC 84 %), no panic, battery 39.5 -> 41.3 C. No sound reached /dev/sweetaudio.
- **daily-verify5-full / daily-verify6-full** (tethered boots of the daily candidates): same pass list; verify6 = the flashed image.
- **wlan-gpu-o-all2** (audio proof, test overlay running `sweetaudio_play both` every 90 s): AudioQueue beeps -> restarted mediaserverd
  (libsalog tap) -> `dev open pid 280` -> amps on, detector MOVED; AudioServices sound completes. Battery 46.2 -> 49.9 C.
- **Nothing had to be dropped**: GPU, audio, Wi-Fi and BT coexist in one boot (shared `sweetenv` holds both injections, `;`-separated;
  cpus 8; payload below the limit).

**Integration findings:**
- *Boot chime*: the up-reg2/3 "chime" was the iOS charge sound, triggered because SweetBattery still showed the static `external=1` when
  SpringBoard started; with Wi-Fi/BT/GPU SpringBoard starts later (+91 s) than the RepSOC publish (+82 s, `external=0`), so no sound plays
  at boot. Not an audio fault (all2), but `external=0` while on USB is a battery-publish gap (open item).
- *Std overlay vs the audio boot job*: upstream's overlay_std puts the root's own launchd.plist back on every boot (ALWAYS_RESET), which
  removed doc 55's `org.pixelios.sweetaudio` job (ringer kick). The daily image now carries its own overlay: root launchd.plist (sha
  7aab4ecb) + that one job (`tools/mk_launchd_job.py --job sweetaudio`), plus the std whiteouts of the pix* test tools (from all1's overlay).
- *Thermal backstop with cpus=8*: patch 0336's X1 drop (>= 48 C) applies mask 0x18 = logical cpus 3,4; with the default /cpus order that
  is one A55 + one A76, so the drop leaves 2 cores (GAP lines say so). It throttles harder than intended but works (all2 at 48-49.9 C).
- *Secrets*: tools/xrun's cred_clean removes `shim/out/shim-boot-avb.img` but not the build intermediates `shim-boot-lz4-avb.img`,
  `shim-boot-lz4.img`, `shim-boot.img`, `ramdisk.bin`, which hold the payload with the WLANCRED blob after a credential build; deleted by
  hand after these runs (upstream gap, not fixed here).

**Daily configuration (daily image 5)** = audio daily args + Wi-Fi/BT/GPU, xrun defaults, no test-only parts (no nav bench, no pixping
overlay, no sweetrestart, no sweetnull, no sweetfps/sweetfbhash):

    image flags: --board oriole --cpus 8 --rw --rails --gui --usbnet --gpu-handover --gpu-clk-level 3 --wlan-hold
                 --wlanfw wlanfw-oriole.bin --aocfw aocf.bin --ampfw ampfw.bin --wlancred <blob, net 2 DHCP>
                 --overlay <root launchd.plist + org.pixelios.sweetaudio, 5 pix* whiteouts>
    args: sweetaoc=15 sweetenv=mediaserverd:DYLD_INSERT_LIBRARIES=/usr/local/lib/libsalog.dylib,SALOG_TAP_NULL=1;backboardd:
          DYLD_INSERT_LIBRARIES=/usr/lib/libsweetgl.dylib,SWEETGL_BOCUT=1,SWEETGL_BOPOOL=1,SWEETGL_FLUSHT=1,SWEETGL_SPLITHEAP=1,SWEETGL_OVERLAP=1
          sweetmali=1 sweetmaliirq=1 sweetmaliovl=1 sweetmalipend=3 sweetmalicache=2 sweetmalipoll=20 sweetwlan=26 sweetwlankeep=1
          + xrun implicit: sweetremote=1 sweetwdt=60 sweetchgi2c=1 sweetchgicl=900 sweetepoch=<build time> sweetusblogport=64738 (+ board touch keys)

Cooler choices (no functional cost observed): GPU pair 3 (572/750 MHz; gpu-o-up1: fps-bench pooled 43.6) instead of pair 1, no MIF request,
no CPU dvfs request, cpus 8 (the count audio/Wi-Fi/BT were verified with; cpus 5 + X1-first order would make 0336's mask exact but is untested
with audio/Wi-Fi and was not worth a boot of the budget). `sweetepoch` is the build time (the clock starts there on every boot; guess:
network time corrects it later). Boot line 864 B (limit 2048). Payload: kernel 17404424 + aocf 3296808 + ampfw 41768 + wlanfw 1804688 +
overlay archive 802816 + ADT ~22 KB + WLANCRED 128 = ~23.37 MB (oriole booted 24.3 MB before). Image `$A/boot-backup/boot_b-ios-daily-820ba87b-full.img`,
sha256 ce54aa752daa045b07006f15c9266f1f867b01475afeebed47f049fd93e89caf, built by `$A/scratch/oriole-all/daily-build.sh` (not in git: it handles
the credential env). It contains the user's Wi-Fi password and stays under ~/pixelios-artifacts.

**Temperatures:** all1 39.5 -> 41.3 C (480 s), verify5 40.4 -> 40.6 C, verify6 42.3 -> 44.7 C (330 s), all2 46.2 -> 49.9 C (420 s, back-to-back);
fastboot storage 44-58 C between boots. Idle lock screen in the full config heats the battery ~0.3-0.6 C/min on USB 2 (charger held at 500 mA).

**Open items:**
- Nav bench: the fixed Settings > General tap does not match this root's Settings list (gpu-o-up1: general-open/about-open 0/3).
- Battery publish `external=0 charging=0` on USB (no charge indicator, no charge sound); USB 2 port keeps CNFG_09 at 500 mA.
- Gate: `tools/check-all.sh` prints `GATE FAIL validate`; the failing lane is the clang-tidy ratchet, identical on pure upstream f084ce5.
- up-reg2 silent stop (log ends ~+155 s, phone off USB 19 min, back in fastboot at 45 C storage): not reproduced, cause unknown.
- 0336 X1-drop mask with cpus=8 (see above); no panic log region (pram) in the ADT.
- Only the user can judge: picture correctness (no camera), sound by ear, Settings > Wi-Fi / Bluetooth UI.

## Step 8: UI integration fixes (2026-11-19; rows ui-diag1..4, daily-verify7-full, daily image 6 in device/google/oriole/docs/PORT.md)

User report on daily image 5 (untethered): GPU fine; Settings shows no Wi-Fi; the Bluetooth switch will not turn on; no sound; volume keys do nothing.
Evidence tool: `tools/ios-uiprobe/pixui_probe` (one-shot launchd job; MobileGestalt answers, MobileWiFi devices/power/scan, BluetoothManager
power as the Settings switch sets it, AVSystemController volume, HID services + dispatched keys, system sounds, AudioQueue tones; counts only).
Boots: 4 diagnostic + 1 verify (of 14).

- **MobileGestalt is not the cause** (*observed* ui-diag1): `wifi=1 bluetooth=1 volume-buttons=1` from the cache on the phone. No userdata reflash
  (the dev overlay delivered everything needed; Step 6 "iOS root" stays unexecuted).
- **Bluetooth: root gap = the chip-boot service** (*observed* ui-diag1 vs ui-diag4). With the stock `com.apple.BlueTool` (`/usr/sbin/BlueTool -R`,
  relaunched every ~31 s, "Device not open yet") bluetoothd never opened its Skywalk channel (no `BT SK: kernel-pipe nexus instance ... ready` in
  bt-up1, verify6 or ui-diag1) and `setPowered:YES` left `powered 0`. Upstream's open `pixos-bluetool` (userland `bluetool/`, root tweak
  `PIXOS_BLUETOOL_BIN`; our root was built without it) installed by the dev overlay (`bluetool/overlay.sh`: only the BlueTool job's ProgramArguments
  change): boot script `result 0`, bluetoothd channel connected, `available 1, powered 1, enabled 1`, scanning on. In daily image 6.
  (libsalog SALOG_OSLOG in bluetoothd crashes it, SIGSEGV in the renderer: do not inject it there; ui-diag2/3.)
- **Sound: the chain was never broken** (*observed* ui-diag1/2/3): AudioServices 1100 (lock) / 1104 (key click) and AudioQueue tones reach the tap,
  `/dev/sweetaudio` and the amps in the full configuration. What changed for the ear: daily image 4 published the static `external=1`, so iOS played
  its connect sound at every boot / plug-in; daily image 5 published `external=0` on USB (FG-current rule, 500 mA net discharge), so iOS never did.
  **Patch 0397**: External/Charging = charger CHGIN_OK (0x69 reg 0xB4 bit 6, one allowlisted read in the 30 s set) or FG current > +50 mA.
  *Observed* ui-diag3 / verify7: `external 1 (CHGIN_OK 1, FG current -358 mA)`, the connect sound plays (`dev open pid 28`, detector MOVED).
- **Wi-Fi: open (location)** (*observed* ui-diag1..4, verify7): the kept-link design hands en0 + the Apple80211 radio to wifid only after the boot
  join (L24..L26) succeeds. Away from the stored network the join ends `L24 STOP ... H5 join set_ssid status=3` (no networks, 31 BSSs seen), the
  engine stops, and wifid has no device (`wifi: devices 0`): Settings shows no Wi-Fi. Within range (Step 3/7 boots) en0 is handed over and
  wifid had `devices=1`. Settings with a live list at this location needs the hand-over without a joined link (keep engine up, link DOWN, 0282
  rejoin attempts and 0292 keep scans; joins of other networks would still be refused) or a real Settings join (host 4-way with a PMK derived from
  the typed passphrase): kernel work on upstream's Wi-Fi stack, not started (Wi-Fi parked, user 2026-11-18).
- **Volume keys: open** (*observed* ui-diag3/4): real kernel presses (patch **0398** `sweethidtestkey=1` on the `sweethidtest` timer) reach
  backboardd (`SWEET HID: key 0xea down (1 clients)`); after 7 presses the next AudioQueue tone had the same tap input peak (23197). Not separated:
  (a) SpringBoard ignores the key, or (b) the volume is applied after VirtualAudio's mix that the tap reads. AVSystemController read from another
  process stayed at 1.000 even after a set, so no in-phone volume witness exists yet. Prepared, not in the daily image: libsalog volume stage
  (notify state `org.pixelios.sweetaudio.volume`, unity without a publisher) + `sweetaudio_vol` (job `sweetvol`). Next: a HUD/volume witness from
  SpringBoard (SBVolumeControl) or VirtualAudio's own volume property, then decide (a)/(b).
- **Tools**: xrun `cred_clean` now deletes the WLANCRED-bearing intermediates too (shim-boot-lz4-avb, shim-boot-lz4, shim-boot, ramdisk.bin);
  `audio/sweetaudio/build.sh` carries the SDK symbol check that lived in the removed `gpu/ios`. Patches 0397/0398 on kernel
  `$A/kernels/kernel-up-deee3753.sweet` (sha256 deee375356b1368c79d9ddca24165975db4604efa3af579b270b27e2d9bfa5ce; 820ba87b stack + 0397 + 0398).
- **Std-overlay ledger trap**: a file a test overlay rewrote (here `/usr/local/lib/libsalog.dylib`) is whited out by the next overlay_std prepare
  unless the run's own overlay carries it; daily image 6 therefore carries the pre-Step-8 libsalog (fa4de81e) itself.

**Daily image 6** = daily image 5 args and flags on kernel deee3753 + own overlay (root launchd.plist + org.pixelios.sweetaudio + BlueTool ->
pixos-bluetool, pixos-bluetool, libsalog fa4de81e); built by `$A/uifix/daily-build6.sh` (not in git: credential env). Image
`$A/boot-backup/boot_b-ios-daily-4413f62a-full2.img`, sha256 4413f62a8696cee8e948c54294eec935e4562d5bcb32719ad3b01e91285d8558, flashed to boot_b
after daily-verify7-full. Still for the user's eyes/ears: the Settings > Bluetooth switch and device list, the connect sound on a charger, lock /
key-click sounds, Settings > Wi-Fi at home (stored network in range at boot), the volume HUD.

## Step 9: sound untethered, volume keys, Wi-Fi status 3 (2026-11-19; rows listen1, vol1, vol2, daily-verify8-full, daily image 7 in device/google/oriole/docs/PORT.md)

User report on daily image 6 (untethered): BT works; Wi-Fi off in Settings; volume keys do nothing; all sounds silent. Later (relayed): "sound works now".

- **Sound: root cause = the AOC load waited 300 s without a USB host** (*read from source*, consistent with every observation). Patch 0376's load
  thread waits (bounded 300 s) for the first USB log datagram (`sweet_usblog_sent`, 0374). Upstream 0184 sends nothing until 192.168.7.1 answers
  ARP, so on a charger or away from the host the counter stays 0: the AOC, amps and `/dev/sweetaudio` came up only ~5 min after boot. Tethered runs
  always log `load start after 0 ms (usb log up)` (verify7, listen1), which is why every tethered check passed. The old audio-only kernel counted
  a send that ARP still held as sent, so it did not wait. **Patch 0399**: wait 3 s by default, `sweetaocwait=<ms>` (<= 300000) for bring-up runs.
  Kernel `$A/kernels/kernel-up-91e56777.sweet` (sha256 91e567779aa0e0d118e496c60162aa2e9fed70ac68e259943c54191a059d2366; deee3753 stack + 0399).
  The untethered effect can only be confirmed off USB (the user's next untethered boot): with USB attached the log is up at once.
- **Levels in the full configuration equal the audio-only image** (*observed* listen1): boot connect sound tap `peak in 4337 out 17268`, kernel
  `peak 8654` (a12p/verify4: 4337 / 17269 / 8654); /dev tone kernel peak 11628; AudioQueue `peak in 23198 out 29196`; system sound `in 6426 out
  25587`. System volume Audio/Video 1.000, Ringtone 1.000, ringer state 1 (not silent); amps power up again after the 30 s idle (`open #N ... amps
  on, detector MOVED` for later opens). Nothing in the merged kernel changed the amp side: the only amp-pad difference vs verify4 is BT_REG_ON
  (gpp16-2, CON16 0x133 / DAT16 7). Objective loudness witness: not built. The 0389 detector reads PCM at the amp input (it scales with the input:
  0x74214 boot chime, 0xa3a80 tone), not the speaker drive. SPK_OUTPUT_POWER needs the DSP in the path (bypassed). A VMON/IMON witness needs
  DSP1 RX3/RX4 back on VMON/IMON plus a calibrated read: open.
- **Volume keys: root cause = no volume control on VirtualAudio's Null_Device route** (*observed* vol1). Physical keys work up to backboardd
  (`SWEET KEYS: key=1/2`, `SWEET HID: key 0xe9/0xea down (1 clients)`; the user pressed during vol1). SpringBoard reacts only with keybag calls
  (`SWEET AKS: SpringBoard sel 8 -> 0xe00002c7`), and no volume change reaches mediaserverd. VirtualAudio: `VolumeProperties for Port: pnul is
  [ ]`, `Device Speaker does not support hardware volume range property`. A `setActiveCategoryVolumeTo 0.25` changes no HAL property
  (SALOG_PROPLOG: only `airs`/`nsrt`/`datc` sets) and no tap level. So iOS never scales the mix the tap reads. **Fix (userland)**:
  `sweetaudio_vol` watches the HID event stream (monitor client) for consumer Volume Increment/Decrement, steps its own volume by 1/16, sets it on
  AVSystemController (best effort) and publishes it to the libsalog tap (volume stage from Step 8, amplitude v^2). *Observed* vol2: `key
  volume-down: volume 0.938 ... tap gain 0.8789`, AQ tone `out 29196 -> 25661`, system sound `25587 -> 19667` at 0.875. In daily image 7 (job
  org.pixelios.sweetaudio-vol, KeepAlive). Not done: the volume HUD (SpringBoard draws none for a route without a volume control; a real
  Speaker route is the proper fix, doc 55 s.4).
- **Wi-Fi boot join status 3 = WLC_E_STATUS_NO_NETWORKS** (bcmevent.h:411, dhd_common.c:4341 "no matching network found"). The cause is location, not
  intermittent (*observed*, counted per boot): the 2026-11-18 boots that joined (wlan-up4, all1) saw 9-19 BSSs with home-router OUIs and the
  stored SSID once each. All five 2026-11-19 boots (ui-diag1..4, verify7: 18-31 BSSs) saw only one campus OUI (00:35:1a, AAU-* networks) and the
  stored SSID 0 times. The credential blob is built the same way for both (mkwlancred, PIXOS_WIFI_NET=2, wpa2-psk). BT was on by default in all1,
  which joined, so coexistence is not the cause.
- **Wi-Fi in Settings without a boot join: open** (*read from source*). Upstream has no Settings flow that works without a boot join. The registry
  objects (0200) are published at boot, but en0, its BSD name, the Apple80211 kept radio and the live scan (0268/0281/0282/0292) exist only after a
  finished L26 (join + ping) in the keep hand-over (`sweet_wlan.c` keep thread). The standalone `sweet_wlan_en0_init` (0199) is "NOT CALLED by
  anything yet". The engine runs as one scripted G2 list (L22..L26): a failed L24 ends it with the undo. A Settings-visible radio without a link
  needs a new engine exit (hand over after L23 with the link down, no flow ring until a later join, a scan-only kept radio) and is kernel work
  on upstream's Wi-Fi stack (Wi-Fi parked, user 2026-11-18). Today: Settings shows Wi-Fi when the stored network is in range at boot (home).
- **Tools**: `pixui_probe seq` (PIXUI_SEQ timed patterns, PIXUI_FINAL_VOL), libsalog `SALOG_PROPLOG=1`, AGENTS.md pointer row + flash-pair approval line.

**Daily image 7** = daily image 6 args and flags on kernel 91e56777 (+0399) + overlay ov-daily7 (daily6 overlay + `sweetaudio_vol` and its job,
libsalog 05b76b40); built by `$A/uifix/daily-build7.sh` (not in git: credential env). Image `$A/boot-backup/boot_b-ios-daily-54909f77-full3.img`,
sha256 54909f7754fcea12a0d9a67f4c87df53a0f07a2f36b73f1191563a2e5b438229, flashed to boot_b after daily-verify8-full. Boots: 4 of 16 (listen1, vol1, vol2,
verify8). User reports (relayed) during this round: "sound works now", "the volume keys are working now". To check by the user: sound within
~2 min of an UNTETHERED boot (charger only), the volume keys change the level, Settings > Wi-Fi at home.

## Step 10: open guest network at boot, volume keys in 3 dB steps (2026-11-19; rows guest1, daily-verify9-full, daily image 8 in device/google/oriole/docs/PORT.md)

User request: away from home the stored network is not in range (status 3, Step 9); join the campus open guest network (no password) at boot.
Later report on the same round: volume keys still make no audible change on daily image 7.

- **Open-network mode did not exist** (*read from source*): mkwlancred refused any security but wpa2-psk, the shim/kernel reader any security word
  but 0; L24 sent `wpa_auth` WPA2_AUTH_PSK / `wsec` AES; L25 required a PMK, the target's RSNE and a finished host 4-way before the en0 hand-over.
- **Added (patch 0400 `sweet-wlan-open-network`, host side ecb83bc)**: `PIXOS_WIFI*_SECURITY=open` (no PSK; a PSK with open is refused) -> blob
  security word 1, psk_len 0, zero psk. Kernel: the reader accepts it without a PMK; L24 builds the same six commands with `wpa_auth 0`, `wsec 0`
  (sup_wpa 0, auth 0 open system, mfp 0, v1 join unchanged) [src OpenBSD sys/dev/ic/bwfm.c:2036-2040, no-privacy branch of bwfm_connect;
  bwfmreg.h:167/180/188]; L25 requires an RSNE-free target record (one with an RSNE is refused), skips the supplicant and the 4-way and sets
  `hs_done`; the 0282 re-join is not armed for an open link (GAP: the link stays down after an AP loss until the next boot); the kept Apple80211
  link answers sec=open (no privacy bit; Get 2 auth lower 1 / upper 0 [guess from the observed WPA2-PSK pair]). WPA2 blobs take the old path.
  Kernel `$A/kernels/kernel-up-027d0dce.sweet` (sha256 027d0dceba25c3df47baba922fc5ec7ec781a7a04bb9ac2199534813289b39aa; 91e56777 stack + 0400).
  Credentials: `$A/wifi-guest.env` (0600, outside git: `PIXOS_WIFI2_SSID`, `PIXOS_WIFI2_SECURITY=open`), used with `PIXOS_WIFI_NET=2` (DHCP);
  `$A/wifi.env` (home network) unchanged. Host tests: `shim/wlan_open_host_test.c` (2 hand mutants caught), test_mkwlancred 18/18.
- **pixping `gw`** (90b5dae): pings the interface's DHCP default gateway (route table), for a network whose gateway is unknown at staging time.
- **guest1** (*observed*, one boot, kernel 027d0dce, the open guest network): `credentials loaded security=open`, `L23 join target is an open BSS
  (no RSNE)`, `L24 join done: SET_SSID status 0, link up`, `L25 done: open network (no keys)`, `A80211 kept link is an open network`, en0 hand-over;
  iOS DHCP lease on en0 (pixping `iface en0 ready addr=172.13.x.x`, gateway from the default route), gateway ping 10/10 (rtt 5-33 ms); pixnet
  9 pass / 0 fail: DNS (one.one.one.one a=2), TCP 443, HTTP to 1.1.1.1:80 status 301 (Cloudflare's own http->https redirect, not a portal page),
  TLS 1.2 trust ok, HTTPS GET status 200 56614 B. **No captive portal and no client isolation seen** from the phone at this location: the guest
  network carried real internet without a browser login. wifid: `devices 1`, power 1, `current network present (joined)`, scan 22 results.
  The user, watching this boot, reported "wifi is working!".
- **Volume keys** (*read from source* + *observed* guest1): the Step 9 agent stepped the tap by 1/16 of v (amplitude v^2): about -1.1 dB per press
  near the top, which is not audible; it also started its HID listener only after the AVSystemController lookup (up to 120 s). The job itself
  ran in the daily image (`exec sweetaudio_vol`, no exit, verify8) and nothing in it waits for USB. New (61a70fa): 16 levels of 3 dB (level 0 =
  mute), one level per press, a log line per press with level and dB, the level kept in `/private/var/root/org.pixelios.sweetaudio.level` and
  restored at start, the HID monitor client started first. guest1: the 0398 test presses (volume-down) and **5 physical presses by the user**
  (`SWEET KEYS: key=1/2` GPIO lines, then `SWEET HID: key 0xe9/0xea down`) each gave `sweetaudio_vol: key volume-up|down: level N/16 (-X dB)`;
  the physical keys and the test presses go through the same kernel HID service (SweetHIDButtons::key, patch 0087/0398). Loudness by ear: user.
  Remaining: the first press can be missed if it comes within ~35 s of the agent start (the HID client came up 35 s after the job started);
  no volume HUD (no Speaker route with a volume control, doc 55 s.4).
- **Credential intermediates**: xrun and the daily build removed the image copies and the blob dir; a stale `wlancred-*` dir from 09:19
  (earlier session, tmpfs) was deleted by hand.

**Daily image 8** = daily image 7 args and flags on kernel 027d0dce (+0400) + the open guest network blob (`$A/wifi-guest.env`, net 2 DHCP) +
overlay ov-daily8 (61a70fa `sweetaudio_vol`); built by `$A/step10/daily-build8.sh` (not in git: credential env). Image
`$A/boot-backup/boot_b-ios-daily-255ccd52-full4-guest.img`, sha256 255ccd52b0e8967dc0ace3083e43af9ea4f8c56a57455bea40c54f5d68e713e0, booted tethered
twice (daily-verify9-full: all markers, the en0 marker matched only a link-local address; daily-verify10-full: DHCP lease on the guest network
+81 s, all 11 markers at +113 s, battery 36.1 -> 36.5 C). **Built, not flashed**: the round was interrupted before the flash; boot_b holds
boot_b-orig.img (recovery) at the start of Step 11. Image 8 is the fallback daily image.


## Step 11: speed (2026-11-19; rows speed-base .. speed-final10 in device/google/oriole/docs/PORT.md)

User report on daily image 8: "The GPU is very slow". Plan woolly-floating-parrot step 1. Boots: 7 of 10. All runs = the full daily configuration
(GPU pair 3 + audio + open guest network Wi-Fi + BT) plus the nav bench (`sweetnav=3`, gpu-fps-bench v2.1), guest WLANCRED, `--gpu-clk-level 3`.

**Panel** (*observed*, patch 0401 `sweette=3`, run speed-base): the primary TE pin (gpp0-3, live DT `te-gpio`) toggles at **58.23 Hz** (TE period
median 17172 us, min 17169, max 17175 over three 1 s windows, pulse ~972 us). DPU dump (0165 mask 1): DECON0 `GLOBAL_CON 0x113 (command mode)
TRIG_CON 0x3061`. The aftermarket panel is a 60 Hz-class panel (not 90 Hz) running 3 % slow; the emulated vsync tick ran 60.00 Hz
(`SWEET VSYNC: n 60 <20:60`, 0148), i.e. ~1.8 copies per second that the panel never showed. Patch **0404** makes the tick period a boot-arg;
the daily set uses `sweetfb_vsync_us=17172`, so the frame ceiling on this phone is 58.2 per second, not 60.

**Bench on this root** (*observed*, 0401/0403 thumbnails of the probed surface): upstream's calibrated taps do not fit the oriole root. 676,1720 is
Photos here (Settings is column 2 row 4, 413,1717; Clock column 2 row 2, 417,1150), and iOS resumes the user's last app (Safari) at the unlock, so
earlier benches (gpu-o-up1, speed-base, speed-free) ran their Settings steps in Photos/Safari. Patches **0403** (`sweetnavset`, `sweetnavclk`,
`sweetnavgy`, labelled thumbnails, `tools/navthumb2png`) and **0405** (`sweetnavhome=1`: home swipe after every unlock) fix it; from speed-mif on
the General probe finds the row and every step is measured. Runs speed-free3 and rep 0 of speed-cpus5 had touch-downs that the bench did not send
(swipes on the home screen during the run: a person or ghost touches of the aftermarket digitizer, not separable from the log): speed-free3 is not
used; speed-cpus5 is also scored on repetitions 1-2 only.

| run | configuration | gpu-fps-bench pooled / slowest step | reps 1-2 pooled / slowest | best drawn s | panel copy | battery start -> nav end (-> idle end) | thermal action / reset |
|---|---|---|---|---|---|---|---|
| speed-base | daily image 8 (sweetfb_hz 30, MIF 421 MHz, cpus 8, no recores) | 36.8 / 14.0 app-launch (Settings steps in Photos) | 35.4 / 11.5 | 30 | 8.7-12.2 ms | 31.5 -> 33.8 (-> 37.1 at +900 s) | none / none |
| speed-free | + free wins (below), cpus 8 recores 0xfe | 42.5 / 22.5 app-close (in Safari) | 41.2 / 26.3 | 53 | 9.7-12.0 ms | 37.3 -> 38.4 | none / none |
| speed-free2 | + sweetnavhome | 40.2 / 18.3 app-launch (Settings steps in Photos) | 41.5 / 23.4 | 46 | 7.1-8.6 ms | 37.8 -> 38.3 | none / none |
| speed-free3 | + Settings/Clock taps | invalid (18 foreign touches) | - | 35 | 9.4-13.8 ms | 38.6 -> 39.8 | none / none |
| speed-mif | + MIF 1352 MHz | 54.3 / 37.3 settings-open | 54.6 / 37.4 | 59 | 2.0-2.6 ms | 39.6 -> 40.0 | none / none |
| speed-cpus5 | + cpus 5, X1-first order, recores 0x1e | 57.5 / 47.0 settings-open | 58.5 / 48.2 | 59 | 1.7-2.3 ms | 39.4 -> 40.1 | none / none |
| speed-final10 | = speed-cpus5 (the daily set), bench + idle to +780 s | **58.5 / 47.1 settings-open** | 58.7 / 45.4 | 59 | 1.5-1.6 ms | 39.9 -> 40.3 (-> 40.5 at +616 s) | none / none |

Free wins = `sweetfb_hz=60` (`--fb-hz 60`), `sweetfb_gate=1` + backboardd `SWEETGL_REALFLUSH=1`, `sweetfb_fifo=1`, `sweetfb_gatekick=1`,
`sweetmalicfg=0x81b00` (END_CLEAN job flush), `sweetfb_vsync_us=17172`, `CA_BACKDROP_SCALE` removed from backboardd's launchd job (overlay
`$A/speed/ov-free`, plist sha b800714f; `pixelios-userland/tools/sweet-root-tweaks.py` no longer sets it). Witness lines: `vsync tick 17172 us`,
`present gate ON`, `swap fifo ON`, `gate kick ON`, `JS_CONFIG_NEXT=0x81b00`, `REALFLUSH pid .. mode 1`, `recommended cores late: requested ..`.
Drawn presents (0368 rows) during animations reach 58-59 per second (best seconds); the per-second mean over every second with frames (35-43/s)
mixes animation and still seconds and is not a frame-rate figure.

**What limited it** (*observed*): (1) the memory clock: the CPU panel copy took 8.7-12 ms per present at the bootloader's MIF 421 MHz, 1.5-2.6 ms
at 1352 MHz (+12 pooled); (2) the core set: with cpus 8 the scheduler (one homogeneous cluster in the ADT) also puts UI threads on the three A55
cores that recores 0xfe leaves in; upstream's cpus 5 X1-first order with recores 0x1e (threads on the 2 X1 + 2 A76 only, cpu0 = A55 for
interrupts) raised the slowest step from 37 to 47 (settings-open = the Settings launch) and pooled to the panel's 58.2 ceiling; (3) the panel
rate itself (58.23 Hz). Not needed: CPU clock raises (`--dvfs`), GPU pair 1. Still below 45 in single repetitions: settings-open rep low1 ~5
(one long frame at the app launch); spotlight-open 53.

**Thermal** (*observed* + host test): patch **0402** (hot mask by topology: the X1 drop keeps the boot mask minus the X1 cores, never fewer
than 3 cores; cpus 8 + recores 0xfe -> 0x3e; cpus 5 X1-first keeps 0336's 0x18 = the A76 pair) was armed in every cpus 8 run (`hot mask 0x3e`)
and the 0336 line in the cpus 5 runs; no run reached 48 C, so no mask fired. `tools/test_sweet_thermal.py` PASS cases=501 (new
test_0402_hot_mask, 6 mutants caught). 55 C CRIT park unchanged.

**Audio / Wi-Fi / BT with cpus 5** (*observed* speed-cpus5, speed-final10): `/dev/sweetaudio up`, connect sound (detector MOVED), BT patchram
203 + bluetoothd, open guest network joined + DHCP lease. The guest join stopped at L24 in 3 of 7 boots (speed-base: `WIFI GAP: scan event data
length 54`; speed-free2 / speed-mif: `set_ssid status=1`), with cpus 8 and cpus 5 alike: an open-network join reliability gap, not a speed effect.

**Daily set (for the daily image build)**: kernel `$A/kernels/kernel-up-1c843b0c.sweet` (sha256
1c843b0c02df8b7ac6ec05dbba60a051d643b34e12d9264d7210652c715bfff3 = 027d0dce stack + 0401..0405; the daily needs 0404, the rest is default off),
`--cpus 5` with mkadt `--cpu-order 0,6,7,4,5,1,2,3`, `--mif-clk-level 1352000`, `--gpu-clk-level 3`, `--gui --fb-hz 60`, overlay `$A/speed/ov-free`,
args = daily image 8 args + backboardd `SWEETGL_REALFLUSH=1` + `sweetfb_gate=1 sweetfb_fifo=1 sweetfb_gatekick=1 sweetrecores=0x1e
sweetmalicfg=0x81b00 sweetfb_vsync_us=17172`. Script: `$A/speed/daily-build9.sh OUT_IMG` (not in git: credential env; all of the above is in it).
No bench args (sweetnav*, sweetfps, sweetfb_copytime/drawn) in the daily image. Daily image 9: Step 12.

**10-minute check** (*observed*, speed-final10): the chosen set ran the full bench and then idled on the lock screen; battery 39.9 C at +76 s,
40.3 C at nav end (+320 s), 40.5 C at +616 s (state NORMAL in every 10 s sample; the host listener stopped at +640 s when the step agent was
stopped, the phone reset itself at +780 s). Well below 48 C, so the fastest configuration measured is also the daily one; no hotter/cooler choice
was needed. Not tried: GPU pair 1, CPU dvfs raises, MIF top row (not needed for the panel ceiling; upstream saw resets at top clocks).

## Step 12: Safari black page, daily image 9 (2026-11-19; rows safari-*, daily-verify11@, daily image 9 in device/google/oriole/docs/PORT.md)

User report on daily image 8: "Safari only renders a black screen". Plan woolly-floating-parrot step 2 (6 boots) + 3. Details and evidence: doc 57 s.6.

- **Cause found** (*observed*): Safari's chrome is composited; the page backing stores WebKit's GPU process draws are IOSurface **'w30r'**
  (CoreGraphics CIF10: x2 R10 G10 B10, extended range) and libsweetgl sampled them as B10G10R10A2_UNORM: alpha = the two zero pad bits, so the
  page tiles blend away to the black window, and the colour values would be biased (384..894). `WKCPU=bitmap` (WebKit's own switch, doc 57 s.3)
  does not change that. Sel 44 and Metal are not involved for page drawing.
- **Fix (userland d0ff1a4, libsweetgl 2fc47f4b on userdata)**: W30R rule in `eagl_mesa.c` (CPU remap copy, alpha 1, render-target and reused-name
  guards). Runs in every boot since safari-w30r-lean without a regression (UI, unlock, bench, audio, BT). **Not proven on screen**: the one clean
  Safari picture (safari-w30r-full, guest join failed, 3 s after launch) is still black; the evidence boot was lost to a silent USB-log stop.
  The user's eyes decide; doc 57 s.6 lists the next boot.
- **Payload**: the dylib cannot ride in a full-configuration image (~28.1 MB, oriole limit between 24.3 and 28 MB): it is delivered by a LEAN run
  and stays on userdata (ledger 2fc47f4b); daily images do not carry it. A later `--intent gpu` run restores the std extra 8efaad3e (no W30R rule).
- **Daily image 9** = daily image 8 (guest-network blob, audio with the 3 s AOC wait, pixos-bluetool, 3 dB volume keys, 0397 charger) on kernel
  1c843b0c with the Step 11 set (cpus 5 X1-first ADT, MIF 1352, `--gui --fb-hz 60`, gate/fifo/gatekick, recores 0x1e, malicfg, vsync 17172,
  REALFLUSH, overlay ov-free without CA_BACKDROP_SCALE); libsweetgl 2fc47f4b from userdata. Built by `$A/speed/daily-build9.sh` (not in git:
  credential env); boot line 995 B. Image `$A/boot-backup/boot_b-ios-daily-8a9cde0a-full5.img`, sha256
  8a9cde0aa60b8598a02ff7bb48b1109eb2ef8d84cbc6c7456fb62cc82fffda66.
- **daily-verify11-full** (tethered, exact file, 784 s): sweetgl +73 s, display on +78 s, lock screen +82 s, `/dev/sweetaudio up` +81 s, connect
  sound `dev open pid 28` +83 s, touch +101 s, bluetoothd +71 s, patchram 203 +111 s, fuel gauge +85 s, MIF `421000 -> 1352000`; **battery 37.1 C
  (+75 s) -> 38.4 C (+616 s) -> 38.8 C (end)**, state NORMAL, no panic. **Wi-Fi: no lease** (`L23 STOP event cap 48 reached`: the scan filled its
  event cap before the target BSS; same in daily-verify12-full). The same kernel, blob and args joined and leased in speed-cpus5 / speed-final10
  and image 8 has the same Wi-Fi code, so this is the open-network join gap at a crowded location (3 of 7 speed boots, safari-bitmap, -full),
  not a property of image 9. fps spot-check: the daily image has no bench args; the identical set measured 58.5 pooled in speed-final10.
- **Flashed**: daily image 9 to boot_b after daily-verify11/12-full (product oriole, slot b, 67108864 B): `Sending 'boot_b' (65536 KB) OKAY`,
  `Writing 'boot_b' OKAY`. Phone left in fastboot. Undo / fallback: image 8 (`boot_b-ios-daily-255ccd52-full4-guest.img`).
- **For the user's eyes**: Safari page area (open a simple page while the guest network is joined; Settings > Wi-Fi shows it); scrolling feel
  (58 fps class on most steps; Settings launch is the slowest step at ~47); sound, BT, volume keys as in image 8; Wi-Fi joins at boot only
  when the guest network wins the scan (L23 event cap gap at crowded places, open item).
