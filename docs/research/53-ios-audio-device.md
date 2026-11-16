# A11: an iOS audio device on oriole (plan only)

Desk note, 2026-11-15. No hardware touched. Tags: **[SRC]** read from source, **[HW]** observed, **[GUESS]** a guess.
Builds on 46 (the AOC audio route) and 50 (Trusty / GSA / AOC transport).

## 1. What the iOS root does today

- `pixelios-userland/tools/sweet-root-tweaks.py:418-423` (`tweak_audio`, commit 1f3877c) **[SRC]**:
  - moves `Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin` to `HAL.disabled/`;
  - sets `StartRecorderServer = false` in MediaToolbox's `SubServerConfiguration.plist`.
- The comment there says the plug-in "cannot load here". The reason is not recorded **[GUESS: it waits on an IOAudio/AppleAudio device or entitlement that does not exist]**.
- So today `mediaserverd` / `audiomxd` have no output device. Any sound API either fails or runs silently.

## 2. Two ways to give CoreAudio a device

| Option | What it is | Effort | Risk |
|---|---|---|---|
| A. Re-enable VirtualAudio | Put the plug-in back. Find out why it did not load (log lines from `audiomxd` / `mediaserverd`), then supply what is missing. | small, if the blocker is only a missing entitlement, preference or IORegistry key | low; no kernel code. A virtual device produces no sound, but it unblocks apps that need *a* device |
| B. Kernel audio device | An IOKit driver that publishes an output engine (IOAudioFamily-style, or the newer `AppleAudio`/AudioDriverKit shape that iOS 16's HAL expects). Its ring is copied into the AOC `audio_playback0` ring (doc 50 §7.4). | large: needs a working AOC (A8), the TDM0 path (A9) and the amp DSP (A9) first | speaker safety (plan rule A-2/A-3) applies to every byte it outputs |

Option A comes first, because it does not depend on the AOC ladder.

## 3. Plan

1. **A11a (root-only, phase 5 dev overlay):** a root with VirtualAudio left enabled. Boot it tethered and grep the log for `VirtualAudio`, `HALPlugIn`, `audiomxd` and `mediaserverd` errors. Record the exact blocker. No kernel change. The run needs the dev overlay (doc 41), so it is a root-writing step and comes last (plan §6 phase 5).
2. **A11b:** fix the blocker found in A11a in the root tweak, and keep the plug-in. Pass: `mediaserverd` lists a default output device and no crash loop in 3 minutes.
3. **A11c (after A8–A10 PASS only):** design an XNU output driver.
   - It publishes one 48 kHz, 16-bit, stereo engine.
   - Its period timer copies frames into `audio_playback0` and advances `tx` / `wp` (doc 50 §6.6, §7.4).
   - Gain is clamped in the driver to the A-3 limit until a reviewed calibration exists.
   - `GLOBAL_EN` stays off unless rule A-2 holds.
   - Needs its own research note on the iOS 16 HAL driver interface (Apple sources are not available; reverse-engineer from the root) and a SAFE review.

## 4. Unknown / how to settle

| # | Unknown | How to settle |
|---|---|---|
| U1 | Why VirtualAudio does not load | A11a log run |
| U2 | Which kernel interface iOS 16 `audiomxd` expects for a hardware device (IOAudioFamily vs AppleAudio user clients) | static look at the root's `audiomxd` / CoreAudio frameworks; no device needed |
| U3 | Default AOC sink gain and units (doc 50 U17) | blocks A11c output, not A11a/b |
