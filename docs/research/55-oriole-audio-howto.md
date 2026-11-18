# 55. Oriole speaker audio: how it works, how to run it (2026-11-18)

Tags: *observed* (run label, PORT.md rows A9-A12), *read from source*, *guess*. Details: doc 54 (contract, VirtualAudio routing).

## 1. Chain (what each stage does)

1. **AOC firmware** (*observed* A6-A8, patches 0164/0165): `aocf.bin` rides in the boot payload (`/chosen/memory-map AOCFW`), the kernel
   copies it into the AOC carveout and asks the GSA to load it (Trusty TIPC, command 90), then starts the AOC; 69 services come up.
2. **AOC EP1** (*observed* a10h, patch 0181): `audio_playback1` (low-latency EP, BIND2 src 1 -> SINK_SPEAKER, S16 48 kHz) is the only
   entry point whose PCM reaches TDM0. EP0 (the stock MMAP EP) and the AOC signal generator output zeros in every format tried.
3. **TDM0 -> CS35L41 L/R** (*observed* a10d-a10h): stock HAL format = 4 x 32-bit slots (TDM_0_RX Chan Four, S32_LE: amp ASP width 32,
   WL 24), R on slots 1/0, PCM gain 17 (stock). Amp bring-up: OTP unpack (0158), HALO DSP firmware from `ampfw.bin` (0169-0171), stock
   DAPM power-up order (0172), DAC fed from ASPRX1 (DSP bypassed, 0181), stock power-down order on idle / shutdown.
4. **/dev/sweetaudio** (patches 0173/0181/0182): 48 kHz S16LE stereo char device, 0666, one writer, 8192-frame FIFO, non-blocking
   write; the kernel feeds the EP1 ring every 1 ms. Gain default -6 dB, max 0 dB, every sample clamped at -3 dBFS (0182). Amps power down
   30 s after the last writer closed and power up again on the next open.
5. **iOS side** (`audio/sweetaudio/`): `SweetAudio.driver` (HAL plug-in, UID `Speaker`, transport `bltn`) gives VirtualAudio a
   Speaker device; VirtualAudio still routes the default category to its `Null_Device` through a HAL aggregate `VAD [vdef] AggDev 1`
   (*observed* a12i-a12m). `libsalog.dylib`, injected into mediaserverd, wraps that aggregate's IOProc and writes VirtualAudio's final
   mix (after its volume) to /dev/sweetaudio: resample 44.1 -> 48 kHz, make-up gain 12 dB with a block peak limiter at -1 dBFS,
   concurrent taps summed, drift trimmed from the kernel FIFO fill (`SWAU_IOC_POS`, <= 0.3 %).
6. **Boot job** `org.pixelios.sweetaudio` (`sweetaudio_install`): waits for SpringBoard's ringer state, then restarts mediaserverd once
   so VirtualAudio builds its routes with the ringer on (*observed* a12i/j).

## 2. Boot-args and files

| Item | Value |
|---|---|
| Kernel | `sweetaoc=15` (implies `sweetamp=1` since 0182); `sweetaudiotest=1` = 2 test beeps at boot (off by default) |
| Userland | `sweetenv=mediaserverd:DYLD_INSERT_LIBRARIES=/usr/local/lib/libsalog.dylib,SALOG_TAP_NULL=1`; optional `SALOG_GAIN_DB=0..24` (default 12), `SALOG_OSLOG=1` (VirtualAudio os_log to stderr; add `sweetnull=mediaserverd` to see it) |
| Blobs (not in git) | `~/pixelios-artifacts/aoc/aocf.bin` (`tools/pack_aocfw.py` of stock `/vendor/firmware/aoc.bin`), `~/pixelios-artifacts/amp/ampfw.bin` (`tools/pack_ampfw.py` of the stock `cs35l41-dsp1-spk-prot.wmfw` + L/R `.bin` tunings, vendor.img `/vendor/firmware`) |
| Root files | `/Library/Audio/Plug-Ins/HAL/SweetAudio.driver`, VirtualAudio.plugin moved back to `HAL/`, `/usr/local/lib/libsalog.dylib`, `/usr/local/bin/sweetaudio_*`, the launchd job; installed once by the dev overlay `ov-sa-driver+va` (`audio/sweetaudio/build.sh driver+va`) and kept on the root |
| Daily image | `boot_b-ios-daily-54a2b214-audio.img` (sha256 a08eba33…, on boot_b since 2026-11-18): daily image 3 args + the two lines above, blobs in the payload, no overlay (the root already holds the files) |

Run (tethered, one phone run at a time):

    audio/sweetaudio/build.sh driver+va [play]          # host test + cross build + overlay (play: test clients after the kick;
                                                        # a third word in the mode file delays them, e.g. "driver+va play 45")
    SWEET_USBLOG_PORT=64738 tools/xrun --board oriole --kernel K --cpus 8 --lock-wait 600 --restart 330,bootloader \
      --aocfw ~/pixelios-artifacts/aoc/aocf.bin --ampfw ~/pixelios-artifacts/amp/ampfw.bin [--overlay DIR] \
      --args 'sweetaoc=15 sweetnull=mediaserverd,sweetaudio_install sweetenv=mediaserverd:DYLD_INSERT_LIBRARIES=/usr/local/lib/libsalog.dylib,SALOG_TAP_NULL=1'

Log lines to look for: `SWEET AUDIO: a12: /dev/sweetaudio up on EP1`, `valog[..]: tap: wrapped IOProc`, `dev open pid`,
`valog[..]: tap: sound: N frames, peak in X out Y`, `open #N evidence: non-zero frames .., peak ..`.

## 3. Key findings (each cost runs)

- EP0 outputs zeros; only EP1 reaches the speaker path (a10h).
- AOC replies may carry another id than the command (EP_SETUP 206 answered with id 288); stock never compares it (0168, a9c/a9d).
- CS35L41 MPU access words read back 0x3, not 0xFFFFFFFF; stock never reads them, our compare was the bug (0171, a9g).
- TDM0 carries 4 x 32-bit slots (stock mixer paths); with the card default S16 the amp got nothing usable (0176).
- VirtualAudio creates a device only for known UIDs (`Speaker`, `Codec`, ...); with UID `Speaker` it builds Device_Speaker and the
  `pspk` port, but its cached default route stays on `Null_Device` (a12h-l). Tapping the aggregate that drives Null_Device is the
  working path (a12n).

## 4. Limits and next steps

- Routing is a tap, not a real VirtualAudio route: SweetAudio.driver's own IO stays idle; latency ~ IO buffer + FIFO (~40-60 ms, *guess*).
- Make-up gain + limiter compress the top 12 dB of iOS volume; volume keys / HUD not tested (no key path checked for audio).
- No microphone, no earpiece/receiver routing for calls (the two CS35L41 amps play as one L/R pair), no calibration (DSP bypassed: no speaker
  protection, hence the -3 dBFS clamp).
- Next: a real VirtualAudio route to the Speaker device (route recomputation or an AVAudioSession client in SpringBoard), volume keys,
  DSP path with stock tuning, microphone (AOC capture EPs), earpiece routing.

## 5. Measured levels (a12p, daily-verify4-audio, observed)

Boot chime: VirtualAudio mix peak 4337 (-17.6 dBFS) -> tap 17269 -> kernel 8654 (-11.6 dBFS; before 0182 / the tap gain: 772).
AudioQueue -3 dBFS test tone: mix 23198 (VirtualAudio at unity for it) -> limiter (gain 2 dB) 29196 -> kernel ~14600. Drift trim
-1181..-370 ppm, kernel underruns 1 per stream start, overruns 0.
