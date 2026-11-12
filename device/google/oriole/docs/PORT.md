# Pixel 6 (oriole) port record

Tags: observed on hardware (with run label and shim build id), read from source, guess.

## Phone

| Item | Value | Tag |
|---|---|---|
| Android build / bootloader | CP1A.260405.005 / slider-16.4-14548185 | observed 2026-11-11 via adb getprop |
| Bootloader after unlock | unlocked, slider-17.0-15681480, slot b (OrangeFox recovery installed by the user) | observed 2026-11-12 via fastboot getvar |
| RAM / UFS | 8 GiB LPDDR5 / 128 GB SK hynix | observed 2026-11-11 via adb getprop |

## Measurements (Part H of the port plan)

| board.json key | Value | Run label | Shim build id |
|---|---|---|---|
| dram_banks | 0x80000000 + 2 GiB; 0x880000000 + 6 GiB (3 x 2 GiB contiguous), no carve-out above 0x880000000 | oriole-probe1 | git 36cacae, sha256 981284da |
| usb_dma_range | abl@f8800000, 16 MiB; GEVNTADR 0xF8D1B000 inside | oriole-probe1 | git 36cacae, sha256 981284da |
| fb_base (text visible on screen) | 0xfac00000, text seen by the user | oriole-probe1 | git 36cacae, sha256 981284da |
| userdata_mib | 112914 (GPT first=0x22F1E6 last=0x1DC03FA, same as bluejay) | oriole-probe1 | git 36cacae, sha256 981284da |

## Gates

| Gate | Result | Run label | Date |
|---|---|---|---|
| G1 probe shim boots, USB log complete | PASS (observed: all 8 CPUs, FDT, UFS GPT read, USB sections; 448 lines) | oriole-probe1 | 2026-11-12 |
| G2 XNU boots, USB Ethernet configured | PASS (observed: Darwin 22.6.0 xnu-8796.141.3 from CI kernel 04457e5 sha256 26d6679c, 1 CPU, USB ECM 192.168.7.2 with DMA in ABL's pool 0xf8d1b000, UFS userdata LUN 0 first block 0x22f1e6 112914 MiB; root mount refused as expected: userdata still Android, HFS signature 0x994d) | oriole-xnu3 (also xnu1, xnu2) | 2026-11-12 |
| G3 iOS lock screen | PASS (observed: iOS root flashed to userdata, sha256 e287716f; 8/8 CPUs, launchd, backboardd +132 s, SpringBoard +139 s, display on +135 s, lock screen SBCoverSheet +561 s, seen on screen by the user; keys driver started; no panic; host reset at --duration 600) | oriole-gui1 | 2026-11-12 |
| G3 touch | NOT YET: FTM5 driver stopped at the AP/AOC mux check (gpp26-1 is an input after the oriole ABL, func=0); next run with sweettouchmux=1 | oriole-gui1 | 2026-11-12 |
| G3 touch | NOT YET: with sweettouchmux=1 the mux reads back driven low (func=0x1 dat=0) and SPI11 is set up, but identify times out (SPI status 0x140, no SCK). Next: the bluejay recipe of xnu patch 0095 (observed on bluejay, run touch1): sweettouchmux=1 sweettouchcs=1 sweettouchpoke="m:10c00640=0=10;d:100;m:10c0180c=5=f;" with --rails | oriole-gui2 | 2026-11-12 |
| G3 touch | NOT YET, but the chip is alive: with the bluejay recipe (sweettouchmux=1 sweettouchcs=1 sweettouchpoke oscillator clock, --rails) the poke reads back (0x10c00640=0x10, 0x10c0180c=0x5), the reset pulse is answered by the FTM5 controller-ready event (`boot ev 03 00 00 00 00 00 00 00`), so SPI clock, pads, mux and rails work. The following DCHIP_ID hw-register read (0x20000000) returns 00 00 00 00 instead of 36 48. Open: why the event-FIFO read works but the hw-register read does not on this unit (guess: register-read dummy byte / timing after the ready event, or the oriole FTM5 firmware o6 vs bluejay b3). | oriole-gui3 | 2026-11-12 |
