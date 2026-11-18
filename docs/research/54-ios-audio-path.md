# A11: fastest path to sound from iOS userland (U2 settled; device contract)

Desk note, 2026-11-18, host only. Tags: **[SRC]** read from the root image / dyld cache / SDK headers, **[HW]** observed, **[GUESS]**.
Follows 53 (plan, U1/U2), 46/50 (AOC route). Evidence files (not in git): `$A/scratch/audio-x/` (Info.plists, `strings` of
`VirtualAudio` = `va.str`, of cache `.13` = `c13.str` (CoreAudio HAL), of `.07` = `c07.str` (MediaExperience)).

## 1. How iOS 16 finds output devices (U2)

- The HAL runs inside **mediaserverd** (user `mobile`, no `audiomxd` binary in this root; its MachServices flip only with feature flag
  `CoreAudioServices/audiomxd`) [SRC `com.apple.mediaserverd.plist`, `rootimg-list.txt`].
- Four device sources in the HAL (`c13.str`) [SRC]:
  1. `HALS_IOA2Manager`: IOKit matching on kernel **`IOAudio2Device`** services (`HALS_IOA2Device: no IOAudio2Device`, `IOServiceAddMatchingNotification`).
  2. `HALS_IOUA*`: AudioDriverKit devices (`com.apple.audio.IOUserAudio`, `registrar.IOUserAudio`).
  3. `HALS_PlugInManager::LoadPlugIns`: **AudioServerPlugIn** bundles (`.driver`) from `/System/Library/Audio/Plug-Ins/HAL` and
     `/Library/Audio/Plug-Ins/HAL` (also `/Library/Apple/Audio/Plug-Ins/HAL`), `loading plug-in from: %s`; optional Info.plist
     `AudioServerPlugIn_LoadingConditions` (IOService matching, deferred load), `AudioServerPlugIn_IOKitUserClients`/`_MachServices`
     (sandbox extensions). The root ships 9 such drivers (AppleUSBAudio, BTAudioHALPlugIn, AppleAOPAudioPlugin, ...), all with plug-in
     type `443ABAB8-E7B3-491A-B985-BEB9187030DB` = `kAudioServerPlugInTypeUUID` of the public macOS header.
  4. `HALS_AHPPlugIn`: the legacy in-process AudioHardwarePlugIn. **VirtualAudio.plugin is this kind** (type `F8BB1C28-BAE8-11D6-...`,
     factory `New_VirtualAudio_PlugIn`); it is not a device driver.
- **VirtualAudio's job** [SRC `va.str`, `c07.str`]: it wraps the physical HAL devices ("PhysicalDevice ... uid") into the virtual devices
  (`VirtualAudioDevice_Default`, `_Music`, `_LowLatency`, ...) and ports (speaker, receiver, USB, BT, AirPlay) that MediaExperience's
  `CMVAEndpointManager` / `vaem*` (AVAudioSession routing, AudioQueue/RemoteIO, system sounds) use. It picks per-product routing and
  transducer databases (`MGGetProductType`; "Product %u will load the unknown routing handlers"); this kernel says iPhone10,3 / D22AP
  (patch 0059) [SRC]. It has a generic USB device class (`Device_USB_Aspen`, "creating a USB output port with name ... and UID ...")
  next to the codec/speaker classes that need Apple's codec ("Could not locate codec device") [SRC].
- Upstream "sweet" did nothing for audio: `tweak_audio` only disables VirtualAudio and the recorder server; README/ROADMAP say audio is not
  done [SRC `pixelios-userland`].

## 2. Recommendation: a user-space AudioServerPlugIn (source 3) + VirtualAudio back on

- Kernel `IOAudio2Device` / `IOUserAudio` emulation means reverse-engineering private user-client protocols (`IOAudio2Notification`,
  `IOAudio2EngineStatus`, control dictionaries) and still leaves VirtualAudio's codec model [SRC strings]. Large.
- An AudioServerPlugIn uses only the public `AudioServerPlugIn.h` (macOS 14 SDK on this host); Apple's own iOS drivers are this kind.
  Loading constraints found: none beyond bundle path + plug-in type in the strings [SRC]; a code-signing / allow-list check inside
  `LoadPlugIns` cannot be excluded without disassembly [GUESS: none, CFPlugIn + dlopen]. This kernel's AMFI marks every validly signed
  file on the root fs as platform binary (`ldid -S` ad-hoc is enough), and the Sandbox policy is permissive (0050) [SRC docs 41, AUTOMATION G5;
  HW: G5 st1a exec + dlopen of overlay-written ad-hoc files]. So `/dev/sweetaudio` mode 0666 is reachable from mediaserverd.
- Device transport: **`usb `** (Info.plist `SweetAudioTransportType`, changeable without rebuild) so VirtualAudio treats it like a
  third-party USB DAC (the class AppleUSBAudio.driver devices land in), not like the D22 codec [GUESS, the U1 run decides].
- VirtualAudio needs to load for apps/system sounds (s.1). Why it "cannot load" (U1) is still unknown; the run order below isolates it.

## 3. Built (host only, not run) [SRC files]

`audio/sweetaudio/`: `SweetAudio.c` (plug-in: 1 device, 1 output stream, Float32 stereo 48 kHz virtual = physical format, WriteMix ->
S16 -> `write()`; no node -> silent sink; logs `sweetaudio: ...` to stderr), `sa_core.h` (+`sa_host_test.c`: conversion, zero time
stamps, write accounting: PASS), `sweetaudio_dev.h` (the contract), `sweetaudio_tone.c`, `sweetaudio_install.c`, `build.sh`.
`build.sh MODE [tone]`: host test, nix clang 21 + theos ld64 for arm64-ios16.0, `ldid -S`, `gpu/ios/check-sdk-symbols.py` RESULT PASS,
Info.plist, overlay dir `$A/audio/ov-sa-MODE[-tone]` + `.bin`. Outputs: `SweetAudio` = Mach-O arm64 bundle, links only CoreFoundation +
libSystem, exports `_SweetAudio_Create`. Root image path: `sweet-root-tweaks.py` honours `SWEETAUDIO_DRIVER=DIR` (installs
`Library/Audio/Plug-Ins/HAL/SweetAudio.driver`) and `SWEETAUDIO_VIRTUALAUDIO=1` (keeps VirtualAudio); default unchanged.

## 4. Kernel <-> userland contract (`audio/sweetaudio/sweetaudio_dev.h` is normative)

| Item | Value |
|---|---|
| Node | `/dev/sweetaudio`, char device (`cdevsw_add(-1)` + `devfs_make_node`, root:wheel **0666**: mediaserverd runs as `mobile`) |
| Format | fixed 48000 Hz, 2 ch interleaved L,R, S16LE; frame = 4 B (the AOC EP0 setup of 0167: 2 ch, 16 bit, 48 kHz) |
| open | `O_WRONLY`, one writer (2nd: `EBUSY`); `ENXIO` when the AOC ring + amps are not up. Kernel brings the path up at boot (boot-arg), open only attaches |
| write | whole frames (`len % 4`: `EINVAL`); **never blocks**: copies `min(len, free)` into a FIFO of 8192 frames (170 ms), returns bytes taken (0 = full; caller drops); `EIO` if the path died |
| drain | the AOC feed (`swau_feed` of 0167, every 1 ms, only what the AOC consumed) takes from the FIFO; starts when >= 960 frames (20 ms) are queued and after each underrun; empty FIFO -> silence + `underruns++` |
| close | FIFO plays out (<= 170 ms), then silence |
| ioctl | `SWAU_IOC_INFO` (`_IOR('a',1)`, version/rate/ch/bits/fifo/start/flags/max_gain_mdb), `SWAU_IOC_POS` (`_IOR('a',2)`, frames played + `mach_absolute_time`, fill, under/overruns), `SWAU_IOC_GAIN` (`_IOW('a',3,int32)` milli-dB, clamped to `[-96000, max_gain_mdb]`), `SWAU_IOC_FLUSH` (`_IO('a',4)`) |
| safety | the kernel owns gain and mute: digital clamp at the plan A-3 limit, `GLOBAL_EN` only under A-2; userland cannot raise it. `SWAU_F_SILENCED` = PCM discarded, writes still succeed |

Latency: HAL IO buffer (512 frames default [GUESS]) + FIFO fill (start 20 ms) + AOC ring; target < 60 ms [GUESS]. Clock: v1 plug-in runs
its own host-clock time line (ZeroTimeStampPeriod 16384); host vs AOC drift ends as rare FIFO over/underruns. v2: drive
`GetZeroTimeStamp` from `SWAU_IOC_POS` (field already in the contract).

## 5. Run recipe (dev overlay 0117/0163; phone runs are the orchestrator's, one new thing per boot)

The overlay may write only `/usr/local`, `/private/var/dev`, the launchd cache: so it stages the bundle at `/usr/local/pixos/SweetAudio.driver`
and adds launchd job `org.pixelios.sweetaudio` (`tools/mk_launchd_job.py ... --job sweetaudio`, from the root's own cache sha256 `7aab4ecb...`).
The job runs `/usr/local/bin/sweetaudio_install`, which reads `/usr/local/pixos/sweetaudio.mode`, copies the bundle to
`/Library/Audio/Plug-Ins/HAL/SweetAudio.driver`, optionally moves VirtualAudio back, and on any change SIGKILLs mediaserverd once
(KeepAlive: launchd restarts it, the HAL rescans). Every step prints `sweetaudio_install: ...`. Base: the G5 xrun `C` list + `--overlay DIR`,
`--args "sweetnull=backboardd,SpringBoard,mediaserverd,sweetaudio_install,sweetaudio_tone"`.

| Run | Overlay | Expect | Tells |
|---|---|---|---|
| sa1 | `ov-sa-driver` | `sweetaudio_install: done driver: changed, ok`, `sweetaudio: factory: created`, `sweetaudio: initialize: transport 0x75736220`, SBCoverSheet | HAL loads an ad-hoc third-party `.driver` (else: loader check, risk 2) |
| sa2 | `ov-sa-driver+va` | as sa1 + `moved .../HAL.disabled/VirtualAudio.plugin`, no mediaserverd crash loop for 3 min, `sweetaudio: start:` when a sound plays | U1: VirtualAudio with a device present; the logged blocker if not |
| sa3 | `ov-sa-driver+va-tone` (needs the kernel node) | `sweetaudio_tone: done: 96000 frames written` | kernel path without CoreAudio |
| sa-restore | `ov-sa-restore`, then the same dir with `--args "sweetdev=restore"` | `done restore: changed, ok`; `restored /System/Library/xpc/launchd.plist` | back to the root-tweak state (created files under `/usr/local` stay, inert) |

Archives (deterministic): `ov-sa-driver.bin` sha256 `e84ca5a2...`, 6 files, 1000336 B data.

## 6. Open risks, ranked

1. **VirtualAudio does not come up on D22 without Apple's codec** (U1): no VADs, so AVAudioSession/system sounds stay silent even with the
   device. Fallbacks: other transport (`bltn`), or a SweetAudio-side "codec" emulation (large), or a different `MGGetProductType` (side effects).
2. An unseen check in `HALS_PlugInManager` (signature / Apple-only bundle ids) [GUESS: none found in strings].
3. mediaserverd's HAL on iOS may insist on properties absent here (box, controls, `kAudioDevicePropertyIcon`, iOS-private selectors); visible as
   `sweetaudio:` lines followed by HAL errors in the tee.
4. Clock drift / FIFO sizing (audible glitches only, not silence); v2 uses `SWAU_IOC_POS`.
5. Kernel side: the device, the FIFO feed into `audio_playback0`, and the A-2/A-3 gain policy are not written yet (kernel lane).

## 7. VirtualAudio routing (runs a12h-a12o, 2026-11-18)

- *read from the binary* (VirtualAudio.plugin of the flashed root; strings + a disassembly annotated on the host, nothing copied
  here): DeviceManager hands each HAL device to a factory keyed by the device UID string (`Codec`, `Baseband Voice`,
  `USB Audio Output`, `USB IDAM`, `Voice`, `Reference`, `Speaker`, `AppleSongbirdDSP`, `Puffin*`, `Actuator`, `AOP Audio-1`,
  `Digital Mic`, `Hawking`, `Halogen`, ..., `Squid Audio`); any other UID gets no VirtualAudio device (A12a-g: `PixOSSweetAudio:0`).
- *observed* (a12i, VirtualAudio's own os_log via libsalog): with UID `Speaker` (transport `bltn`) VirtualAudio builds
  `Device_Speaker` on SweetAudio.driver (hog mode, a tap input stream of its own, ports `pspk`, `pspr`, `pstp`), but the
  default route (category csav, RoutingSettings_N71) stays on its `Null_Device`: computed while the ringer state reads off
  (`Culling route connection because ringer switch is off`) or before the Speaker device exists, and kept afterwards
  (`Redundant concrete route(s)`; port updates only look for a replacement when the current route is invalid). A restart of
  mediaserverd after SpringBoard published the ringer state (a12j), an AVAudioSession Playback category from a command-line
  client (a12k, still csav) and hiding Null_Device from 'dev#' reads (a12l, VirtualAudio never reads 'dev#' itself) did not
  change it.
- *observed* (a12n): VirtualAudio drives that route through a HAL aggregate (`VAD [vdef] AggDev 1`, 44.1 kHz, Null_Device as
  sub-device) created with the public AudioDeviceCreateIOProcID. libsalog (`SALOG_TAP_NULL=1`) wraps that IOProc and writes the
  final mix (after VirtualAudio's volume) to /dev/sweetaudio: AudioQueue, an AudioServices system sound and a boot-time system
  sound reach the kernel (detector MOVED). This is the working path; it needs
  `sweetenv=mediaserverd:DYLD_INSERT_LIBRARIES=/usr/local/lib/libsalog.dylib,SALOG_TAP_NULL=1` (patches 0110 + 0106).
- Limits [GUESS]: only `VAD [` aggregates are tapped (alternate VADs created later are tapped too, but two playing at once
  would interleave in the FIFO); host clock vs AOC clock drift ends as rare FIFO drops; SweetAudio.driver's own device stays idle.
- Debug: `SALOG_OSLOG=1` prints every os_log message of mediaserverd to stderr (`sweetnull=mediaserverd` tees it).
- How-to, boot-args, levels and limits after the polish pass (a12p, 2026-11-18): doc 55.
