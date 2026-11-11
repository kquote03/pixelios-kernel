# Pixel 6 (oriole) port record

Tags: observed on hardware (with run label and shim build id), read from source, guess.

## Phone

| Item | Value | Tag |
|---|---|---|
| Android build / bootloader | CP1A.260405.005 / slider-16.4-14548185 | observed 2026-11-11 via adb getprop |
| RAM / UFS | 8 GiB LPDDR5 / 128 GB SK hynix | observed 2026-11-11 via adb getprop |

## Measurements (Part H of the port plan)

| board.json key | Value | Run label | Shim build id |
|---|---|---|---|
| dram_banks | TBD | TBD | TBD |
| usb_dma_range | TBD | TBD | TBD |
| fb_base (text visible on screen) | TBD | TBD | TBD |
| userdata_mib | TBD | TBD | TBD |

## Gates

| Gate | Result | Run label | Date |
|---|---|---|---|
| G1 probe shim boots, USB log complete | TBD | | |
| G2 XNU boots, USB Ethernet configured | TBD | | |
| G3 iOS lock screen and touch | TBD | | |
