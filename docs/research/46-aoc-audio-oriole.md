# A0: speakers on oriole (AOC, GSA, CS35L41)

Research only, no hardware touched. Date 2026-11-14.

Tags: **[HW]** = observed on hardware, **[SRC]** = read from source (file:line at the named commit), **[GUESS]** = guess.

Sources used:
- `aoc` = kernel/google-modules/aoc, branch android-gs-raviole-6.1-android16, commit `ac69293`
- `amp` = kernel/google-modules/amplifiers, same branch, `33976d7`
- `gsa` = kernel/google-modules/soc/gs, same branch, `2040787`, under `drivers/soc/google/gsa/` and `include/linux/gsa/`
- `dts-gs101` = devices/google/gs101 `a9b5edb` (tag android-16.0.0_r0.10), under `dts/`
- `dts-rav` = devices/google/raviole `81506b9`
- `regmap-spi` = torvalds/linux v6.18, `drivers/base/regmap/regmap-spi.c`
- `ref` = stock recovery capture `~/pixelios-artifacts/ref/oriole/20261114T072643Z/` (dmesg.clean, pinctrl-pins.txt, regulators.txt, dt/), taken 2026-11-14 in OrangeFox recovery on kernel 6.1.162-android14-11-ab16261587
- `vendor.img` = `~/pixelios-artifacts/stock/oriole/extract/vendor.img` (factory cp1a.260405.005); file names listed read-only with 7z

## Short answer

- Every PCM sample to the speakers passes through the AOC (Always-On Compute) co-processor.
  - The AP controls the two CS35L41 amps over SPI only (register access and DSP firmware).
  - The amps' audio bus (TDM0) and its bit clock come from the AOC. **[SRC]**
- Starting the AOC needs two things:
  - a signed `aoc.bin`, authenticated by the GSA (Google Security Anchor, the secure processor) through its mailbox;
  - a start command sent to a **Trusty TEE service** (`com.android.trusty.gsa.hwmgr.aoc`) over Trusty IPC. **[SRC]**
- So XNU needs a GSA mailbox client, a Trusty IPC client and the AOC IPC protocol before any sound can play.
- A read-only identify of the amps (DEVID/REVID over SPI13) is cheap and safe, and can be done first.

## (a) Sequence from "AOC off" to "a PCM stream is accepted by the amps"

1. **Kernel probe of the AOC core** (`aoc_core.ko`, compatible `google,aoc`).
   - **[SRC]** `dts-gs101/gs101-aoc.dtsi:28-87` declares:
     - registers: `blk_aoc` 0x19000000 (16 MiB), `aoc_req` 0x175D0000, `aoc_s2mpu` 0x1A0B0000, `ssmt_aoc` 0x1A060000;
     - 16 mailbox channels on `mbox_aoc` (0x176a0000, `google,mailbox-whitechapel`);
     - `gsa-enabled`, `gsa-device = <&gsa>`, `memory-region = <&aoc_reserve>`, `acpm-ipc-channel = <13>`.
   - **[HW]** `ref` dmesg:2067: "found aoc ... sram:[mem 0x19000000-0x19ffffff] dram:[mem 0x94000000-0x96ffffff]". The reserved memory is `aoc@94000000`, 48 MiB (dmesg:21).
   - The probe:
     - **[SRC]** configures the SSMT bypass (`aoc/aoc_v1.c:317-338`) and clears AOC GPIO interrupts (`aoc_v1.c:246-249`);
     - **[SRC]** does not load firmware unless `aoc_autoload_firmware` is set, and it defaults to false (`aoc/aoc.c:103`, `2703-2707`).
2. **Userspace starts the load.**
   - **[SRC]** `vendor.img` `/vendor/etc/init/init.aoc.rc` contains only "AoC firmware will be loaded by Daemon".
   - **[SRC, strings of the Google binary]** `/vendor/bin/aocd` holds the strings `/sys/devices/platform/19000000.aoc/firmware`, `/vendor/firmware/` and `aoc.bin`.
   - Writing a name to the sysfs file `firmware` calls `start_firmware_load()` → `request_firmware_nowait(...aoc_fw_callback)` (`aoc.c:898-906`, `917-940`).
   - The default name is `aoc.bin` (`aoc.c:102`).
   - **[SRC]** `vendor.img` holds `/vendor/firmware/aoc.bin`, 15,158,976 bytes.
3. **`aoc_fw_callback()`** (`aoc.c:472-674`), in this order:
   1. `request_aoc_on(1)` writes 1 to `aoc_req` (`aoc.c:557`, `aoc_v1.c:87-90`).
   2. It validates the image (`_aoc_fw_is_valid`, `_aoc_fw_is_compatible`) and refuses unsigned images: "Loading unsigned aoc image is unsupported" (`aoc.c:600-604`).
   3. It copies the image body into the DRAM carveout at `AOC_BINARY_DRAM_OFFSET`, which is 0 (`aoc.c:613`, `aoc.h:318-320`).
   4. Because `gsa-enabled` is set, it calls `aoc_fw_authenticate()` (`aoc.c:619-630`, `441-470`):
      - it copies the first `AOC_AUTH_HEADER_SIZE` = 4096 bytes (`aoc_firmware.h:16`) into a DMA buffer;
      - it calls `gsa_load_aoc_fw_image(hdr_dma, carveout_base + 0)`. See (c).
   5. It writes a key/value parameter block (board id/rev, carveout address and size, heap addresses, chip revision, random seed, …) into AOC DRAM at the IPC offset (`aoc.c:503-526`, `635`).
      - The board values come from DT: `aoc-board-cfg = "o6"`, `aoc-board-id = <0x20302>`, `aoc-board-rev = <0x10000>` (`dts-rav/gs101-oriole-audio.dtsi:12-16`).
   6. It sets `aoc_state = FIRMWARE_LOADED` (`aoc.c:638`) and keeps the AP out of sleep for 2 s.
   7. It sends `gsa_send_aoc_cmd(GSA_AOC_START)` (`aoc.c:646-651`), then arms a 5 s "online" monitor (`aoc.c:660`).
      - Without GSA, it would set bit 0 of the PCU reset register instead (`aoc_v1.c:70-85`). That path is not used on oriole.
4. **The AOC comes online.**
   - The AOC firmware raises a mailbox interrupt.
   - `aoc_mbox_rx_callback` sees `FIRMWARE_LOADED && aoc_fw_ready()` and schedules `aoc_did_become_online` (`aoc.c:341-346`).
   - That function clears `aoc_req`, checks and creates up to 200 service devices, and sets `AOC_STATE_ONLINE` (`aoc.c:1323-1395`). **[SRC]**
5. **The ALSA card binds.** `aoc_snd_card_probe` waits for the AOC service `CMD_OUTPUT_CHANNEL` and returns -EPROBE_DEFER until it exists ("wait for aoc output ctrl", `aoc/alsa/aoc_alsa_card.c:1910-1918`). **[SRC]**
   - **[HW]** In the recovery capture the AOC was never started:
     - there is no "attempting to load firmware" line;
     - the card logged "wait for aoc output ctrl" from 3.87 s to 37.4 s and "platform sound-aoc: deferred probe pending" (`ref` dmesg:2639-5331, 3005);
     - "aoc hunt: vendor mount FAILED" (dmesg:3479).
   - **This capture is not a reference for a running AOC.**
6. **The amps are probed on the AP**, independently of the AOC. `cs35l41 spi13.0/.1`:
   - they defer at first ("Failed to request core supplies: -517") until S2MPG11 BUCKA appears, then print "Cirrus Logic CS35L41 (35a40), Revision: B2" (`ref` dmesg:1289-1326, 1641, 1813). **[HW]**
   - The probe order is in (d).
7. **The route is set up (ALSA DPCM).** Front end `audio_playback0` (EP1) → back end `TDM_0_RX` (`dai-format = "dsp_a"`) → codecs `cs35l41_l` and `cs35l41_r` (`dts-gs101/gs101-audio.dtsi:53-61`, `673-687`; `gs101-raviole-audio.dtsi:15-27`). **[SRC]**
   - The back end's CPU DAI is `aud_aoc_path` (`google-aoc-path`). Its `hw_params` only logs (`aoc/alsa/aoc_alsa_path.c:928-936`).
   - The real port setup is an AOC command: `aoc_audio_path_open()` / `CMD_AUDIO_OUTPUT_EP_SETUP(2)` (`aoc/alsa/aoc_alsa_hw.c:1635`, `1692-1694`), driven by the mixer controls `TDM_0_RX Mixer` (`aoc_alsa_path.c:1457`, `2019`).
8. **DAPM power-up of each amp:**
   - wm_adsp loads the DSP firmware over SPI: `cs35l41-dsp1-spk-prot.wmfw` plus the `.bin` tuning files. The name pattern is `"%s-%s-%s.wmfw"` and `"%s-%s-%s.bin"` (`amp/snd_soc_wm_adsp/wm_adsp.c:1848`, `2596-2600`).
   - **[SRC]** `vendor.img` `/vendor/firmware/` holds `cs35l41-dsp1-spk-{prot,cali,diag}.{wmfw,bin}` and `R-cs35l41-dsp1-spk-{prot,cali,diag}.bin`.
   - Then `cs35l41_main_amp_event` POST_PMU applies `cs35l41_pup_patch` and sets `PWR_CTRL1` (0x2014) `GLOBAL_EN` = 1 (`amp/cs35l41/cs35l41.c:2279-2295`; `cs35l41.h:42`, `667`).
   - The amp clocks from the TDM bit clock: `src = "BCLK"` for both amps (`gs101-raviole-audio.dtsi:35-62`). So the AOC must already be driving TDM0.
9. **PCM data:** the AP writes samples into the AOC service ring `audio_playback0` (`aoc_alsa_pcm.c:681` `aoc_audio_write`, `aoc_alsa_drv.c:37`). The AOC DSP mixes them and puts them on TDM0 to the amps. **[SRC]** for the ring; **[GUESS]** for the AOC's internal mixing.

## (b) Does the AP have its own I2S/TDM path to the CS35L41?

- **No.** Every back-end DAI in the card (`I2S_0..2`, `TDM_0..1`, BT, USB, haptics) names `aud_aoc_path` as its CPU DAI (`gs101-audio.dtsi:576-866`). **[SRC]**
- The gs101 DT has no AP-side I2S, ABOX or PCM controller node: a grep of `gs101.dtsi`, `gs101-pinctrl.dtsi` and `gs101-pm-domains.dtsi` for i2s/abox/tdm/pcm finds none. **[SRC]**
- The only other codec node is `max98357a`, and it is `status = "disabled"` (`gs101-audio.dtsi:874-877`). **[SRC]**
- **[HW]** None of the 8 pin controllers in `ref` (0x10840000, 0x10C40000, 0x11840000, 0x14440000, 0x174D0000, 0x174E0000, 0x17940000, 0x17A80000) carries a TDM function to the amps.
  - The AP-visible amp pins are only SPI13 (gpp25-0..3, function 2), CS2 (gpp1-0), the resets (gpp16-3, gpp23-7) and the IRQs (gpa6-3, gpa9-6).
  - **[GUESS]** The TDM pads are in the AOC's own GPIO block (`gpio-base = <0xB70000>` inside `blk_aoc`, `gs101-aoc.dtsi:78`).
- **[GUESS]** Remapping those pads so that an AP-side controller drives them is not shown anywhere in the sources, and gs101 has no AP-side audio serial controller. A "direct path" would therefore mean AOC-free bit-banging or a custom AOC firmware. Neither is realistic.

## (c) What loading AOC firmware needs

- **Firmware file:** `/vendor/firmware/aoc.bin` (15,158,976 bytes, in `vendor.img`). **[SRC]**
  - It must be the **signed** build: the driver rejects unsigned images (`aoc.c:600-604`), and the GSA checks the signature. **[SRC]**
  - Keep it outside git (AGENTS rule 4). It is not an Apple binary, but it is firmware.
- **Header handling:** the 4 KiB auth header (magic `AOC_AUTH_HEADER_MAGIC_VALUE 0x00434F41`, `aoc_firmware.c:13`) goes in DMA memory that the GSA can read. The image body goes at the physical base of the AOC carveout. **[SRC]**
- **GSA mailbox step: load and authenticate.** `gsa-ns` mailbox at 0x17c90000 (`dts-gs101/gs101-gsa.dtsi:3-12`). **[SRC]**
  - Command `GSA_MB_CMD_LOAD_AOC_FW_IMG` = 90 (`gsa/drivers/soc/google/gsa/gsa_mbox.h:50`), with 4 arguments: header DMA lo/hi, body PA lo/hi (`gsa_core.c:82-95`, `gsa_mbox.h:90-96`).
  - Mailbox protocol (`gsa_mbox.c:288-332`):
    1. SR0 = cmd, SR1 = argc, SR2.. = args (`MBOX_SR_REG(n)` = 0x80 + 4n);
    2. unmask and clear INTMR0/INTCR0 bit 0;
    3. ring `INTGR1` (0x40) bit 0;
    4. wait for the response IRQ;
    5. the response is SR0 = cmd|0x80000000, SR1 = err, SR2 = argc, SR3.. = args.
  - **[SRC]** After this call the AP can no longer write the image body. `gsa_aoc.h:26-41`: the GSA copies in and authenticates the header, then locks the body region; the buffer "becomes inaccessible for duration of this call and remains inaccessible after".
  - **[GUESS]** The lock is done through the AOC S2MPU (`aoc_s2mpu`, 0x1A0B0000).
- **Trusty step: start.**
  - `gsa_send_aoc_cmd()` does not use the mailbox. It sends `HWMGR_CMD_STATE_CMD` with `GSA_AOC_START` (=1) over a Trusty IPC channel to the port `com.android.trusty.gsa.hwmgr.aoc` (`gsa_core.c:97-131`, `187-194`, `851`; `hwmgr-ipc.h:10-37`; `gsa_aoc.h:65-86`). **[SRC]**
  - The other commands: `GET_STATE` = 0, `SHUTDOWN` = 4, `RELEASE_RESET` = 5, `RESET` = 6. Unload is `HWMGR_CMD_UNLOAD_IMG`, also over Trusty. **[SRC]**
  - **[HW]** Trusty is present on oriole: "trusty version: Project: slider, Build: 15644732", API version 5 (`ref` dmesg:642-643). The DT has `android,trusty-smc-v1` with irq/log/virtio children (`gs101-trusty.dtsi:12-24`).
  - **[GUESS]** XNU would need a minimal trusty-virtio/TIPC client: SMC calls, virtio vrings in shared memory, and a channel connect by port name. That is a substantial new component.
- **Other prerequisites:**
  - **[SRC]** ACPM IPC channel 13 (AOC power and DVFS) and the `aoc_req` handshake;
  - **[SRC]** the AOC sysmmu (`iommus = <&sysmmu_asoc>`; `ref` dmesg:2044 "has sysmmu 1a090000.sysmmu") and its fault handler;
  - **[GUESS]** that the GSA and Trusty accept a load request from a non-Linux kernel. Nothing in the source ties them to Linux, but the TEE may check state set by the bootloader that we cannot see.
- **After start, the protocol:**
  - **[SRC]** service discovery in DRAM;
  - **[SRC]** a 16-channel mailbox at 0x176a0000;
  - **[SRC]** the audio control service `audio_output_control`, with `CMD_AUDIO_OUTPUT_*` command structs from `aoc/aoc-interface-gs101.h`, plus ring buffers.
  - Getting the speakers working means re-implementing roughly `aoc.c`, `aoc_service_core.c` and part of `alsa/aoc_alsa_hw.c`. **[GUESS]**

## (d) cs35l41_probe order and a read-only identify

**Probe** (`amp/cs35l41/cs35l41.c:4154-4430`, `33976d7`), all **[SRC]**:

1. `devm_regulator_bulk_get` and enable of the supplies "VA" and "VP" (`:62-65`, `:4170-4206`).
   - VA = `s_bucka_reg` (S2MPG11 BUCKA). VP = `V_SYS_dummy`, a fixed 4.2 V always-on dummy (`gs101-raviole-audio.dtsi:251-252`, `293-294`; `gs101-audio.dtsi:15-22`).
   - **[HW]** S2MPG11_BUCKA is enabled at 1,850,000 µV (`ref` regulators.txt). Its DT entry is always-on, 1.7-2.1 V (`gs101-pmic.dtsi:907-913`).
2. `reset-gpios` is requested as `GPIOD_OUT_LOW`, which asserts reset (`:4210-4211`). Then 2 ms → set 1 (release) → 2 ms (`:4224-4230`).
   - The pins are `gpp16-3` for L and `gpp23-7` for R, both active-high flag 0 (`gs101-raviole-audio.dtsi:249`, `291`).
   - **[HW]** In recovery both pins are CON 0x1 (output), DAT 0x1, so out of reset (pinctrl-pins.txt:57, 91).
3. Poll `IRQ1_STATUS4` (0x0001001C) for `OTP_BOOT_DONE` (bit 1, 0x02), every 1 ms up to 100 times (`:4232-4242`; `cs35l41.h:127`, `690`).
4. Read `IRQ1_STATUS3` (0x00010018). If `OTP_BOOT_ERR` (0x80000000) is set, fail (`:4244-4249`; `cs35l41.h:126`, `693`).
5. Read `DEVID` (0x00000000) and `REVID` (0x00000004) (`:4251-4261`; `cs35l41.h:24-25`).
   - `mtl = REVID & 0x0F`.
   - The expected DEVID is 0x35A40 (CS35L41) when `mtl` is even, or 0x35B40 (CS35L41R) when odd (`:4263-4274`; `cs35l41.h:727-729`).
6. IRQ setup, then the first **writes**:
   - IRQ masks (`:4276-4310`);
   - the errata patch per revision; B2 → `cs35l41_revb2_errata_patch` (`:4351-4362`);
   - `cs35l41_otp_unpack` (`:4385`);
   - `DSP1_CCM_CORE_CTRL` = 0 (`:4401`);
   - DSP init and codec registration (`:4407-4414`).
7. Log "Cirrus Logic CS35L41 (%x), Revision: %02X" with `regid, reg_revid` (`:4422`).
   - **[HW]** Stock prints `(35a40)` and `B2` for both spi13.0 and spi13.1. So DEVID = 0x00035A40 and REVID = 0xB2 (`mtl` = 2, even, so CS35L41). That matches `CS35L41_REVID_B2 = 0xB2` (`cs35l41.h:732`).
   - Note that `%02X` would print every set bit, so REVID is exactly 0xB2, with nothing above the low byte. **[SRC]**

**SPI framing** (`amp/cs35l41/cs35l41-spi.c:32-45`): 32-bit register address, 16 pad bits, 32-bit value, big-endian, stride 4. Bus: `spi_13` = 0x10D60000, USI13 in PERIC1, `samsung,usi-offset = 0x14` (`gs101-usi.dtsi:644-662`); 25 MHz max; CS0 = gpp25-3, CS1 = gpp1-0, both active-low (`gs101-raviole-audio.dtsi:223-240`).
- **[SRC]** `regmap-spi` sets `.read_flag_mask = 0x80` (`regmap-spi.c:107`, v6.18). So a read sets bit 31 of the address.
- **[GUESS]** That this applies unchanged in the 6.1 Android kernel. It has to be confirmed with the first read, which must return 0x00035A40.

**Read-only identify (proposal, not built):**
1. Configure SPI13 pins and the USI13 mode, with CS manual or auto, mode 0. **[GUESS]** The SPI mode is not set in the DT, so it is the spi core default, mode 0.
2. Do not touch the reset lines if they read high.
3. Read 0x0001001C and check bit 1.
4. Read 0x00000000 and expect 0x00035A40. Read 0x00000004 and expect 0x000000B2. Optionally read OTPID at 0x00000010 (`cs35l41.h:28`). Do this on CS0 (L) and on CS1 (R).

- This is reads only. No `PWR_CTRL1`, no errata, no DSP.
- The amp's output stage stays off: `GLOBAL_EN` is only set at DAPM POST_PMU (`cs35l41.c:2293`).
- **[GUESS]** A hibernated amp (`cirrus,hibernate-enable`) may need a wake sequence before it answers. If the reads return 0 or 0xFFFFFFFF, stop: do not pulse reset without a separate review.

## (e) Decision memo: AOC path, direct path, or defer

| Option | What it takes | Risks |
|---|---|---|
| **AOC path** (the only path the hardware supports, **[SRC]**) | GSA mailbox client, Trusty TIPC client (hwmgr.aoc START), AOC IPC and services, AOC audio command set, CS35L41 SPI driver with wm_adsp firmware and tuning loading, an IOAudio/CoreAudio driver in XNU | Very large; three opaque firmwares (aoc.bin, Trusty, GSA); a wrong AOC command set can configure TDM/boost badly; a failure can wedge the AOC and need a reboot |
| **Direct AP path** | Does not exist: no AP I2S/TDM controller to the amps (**[SRC]**, see (b)) | n/a |
| **Defer** (recommended) | Do the read-only DEVID/REVID identify only (one probe, reads only) | Minimal; SPI reads cannot enable the output stage |

**Speaker safety, whatever the option:**
- Never set `GLOBAL_EN` (0x2014 bit 0) without the Cirrus speaker-protection DSP firmware (`cs35l41-dsp1-spk-prot.wmfw` plus tuning and calibration `.bin`) and the boost limits from the DT (`cirrus,boost-peak-milliamp = 3500`, `boost-ind-nanohenry = 1000`, `boost-cap-microfarad = 30`; `gs101-raviole-audio.dtsi:254-256`). **[SRC]** for the values.
  - **[GUESS]** Running without protection allows the coil to overheat or over-excurse.
- The user's rule: **no sound output is enabled without a listener present.** Any first playback must be low amplitude, short and attended. That needs its own reviewed `wip/*` branch (AGENTS rule 8).
- **[GUESS]** The haptics amp CS40L25 on hsi2c_8 at 0x43 (`gs101-raviole-audio.dtsi:162-221`) is a separate actuator. Do not touch it in any audio test.

**Recommendation: defer speakers.**
1. Allow one read-only identify step (SPI13 reads of 0x0, 0x4, 0x1001C on both chip selects; reviewed by a different model family).
2. Record the result here.
3. Revisit the AOC path only after the Trusty IPC client exists for another reason (it is also needed for TPU/DSP on gs101, **[SRC]** `gsa_core.c:851-853`).
