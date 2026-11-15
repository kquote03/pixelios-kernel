# oriole battery and charging: status and backlog

Status 2026-11-15: **DONE for now** (user decision). Claims are tagged *observed* (on the phone), *source* (read from source) or *guess*.

## What works

| Item | State | Evidence |
|---|---|---|
| Fuel gauge read under XNU (MAX77759 0x36 on HSI2C12, patch 0148, `sweetbatt=1`) | works | c4-batt6, c5-pub1, c6-chg2, daily-verify2 (observed) |
| Battery % in iOS (patch 0153, SweetBattery publishes the gauge every 30 s) | works | user saw ~35 % on the iOS indicator, c5-pub1 (observed) |
| Charger status read (0x69: INT_OK, DETAILS_00..03, CNFG_00/02/09, `sweetbatt=2`, read-only) | works, diagnostic only | c6-chg2 (observed) |
| Daily image on boot_b with `sweetbatt=1` | flashed | `~/pixelios-artifacts/boot-backup/boot_b-ios-daily-a1a9846d-batt.img` (sha256 1502dd8c…), PORT.md "daily image 2" |
| Reviews | all SAFE | docs/reviews/0148-hsi2c-fg-review-fable-3.md, 0148-0153-c5-c6-review-fable-4.md, 0148-0153-review-fable-5.md |

Known fact behind "not charging": the charger charges (mode 0x5, VBUS valid, fast charge CC) but `CHG_CNFG_09` CHGIN_ILIM = 0x13 = 500 mA input
(observed + source BMS max77759_charger.c:1519-1527). iOS draws more (about 0.44 A from the battery on a laptop port, guess: ~4 W system load, 8 cores,
no CPU idle/DVFS), so the battery discharges. Stock recovery shows the same 500 mA class input ("main-charger Charging at 473500 uA", I1 reference
capture, observed).

## Backlog (in priority order)

1. **Net charging.** Two routes, user decision:
   - a. Lower iOS power: CPU idle states / DVFS governor, display and GPU power. No SAFETY.md conflict.
   - b. Raise the input limit: a `CHG_CNFG_09` write. SAFETY.md forbids charger writes unless the user lifts it. If lifted: first a read-only
     Type-C advertisement read (TCPC 0x25 CC status, BC1.2), then set the limit to what the source offers, Fable review first. A USB 2.0 laptop
     port may only allow 500 mA anyway; a USB-C wall charger benefits most.
2. **Real charging state in iOS.** ExternalConnected / IsCharging are inferred from AvgCurrent > +50 mA (wrong while plugged in and discharging,
   review fable-4 F2). Use the C6 bytes (CHGIN_STS, CHG_DTLS) instead. Before `sweetbatt=2` goes into the daily image: fable-4 F1 (more messages per
   set = more chances for a latched stop) and a few more runs without `refused`.
3. **Test on the phone:** the stale-keep path of 0153 (after a reader stop iOS keeps the last real values, `SweetBatteryStale`), reviewed, never run.
4. **Low-battery behaviour.** `setAtWarnLevel(false)` and `setAtCriticalLevel(false)` always: no warning, no clean shutdown before the cell empties.
   Decide thresholds (powerd) from the gauge once the % is trusted. TimeRemaining stays -1, CycleCount 0.
5. **Review leftovers** (all low severity, no phone-safety issue):
   - fable-5 F2: mark stale publishes in the log (`published (stale)`) and pass `currentValid = false` for them.
   - fable-5 F4: host test for a silent NACK (state 0xB, INT_STATUS 0, no data); the fake always raises NO_DEV_ACK.
   - fable-3 H1 / fable-5 F3: unknown whether gs101 INT_STATUS shows masked error bits (INT_EN = TRANS_DONE only); a real NACK may only appear as `timeout`.
   - fable-4 F3/F4: audit pins for the C5/C6 glue (charger reads only with `sweetbatt=2`, `sweet_batt_sample` never calls `sfg_set`, the sample honours
     `stopped`, only 1|2 accepted) and the 8-register list as one pinned string.
   - fable-4 F5: confirm "no read side effect" of 0xB4..0xC2 from the MAX77759 datasheet (memo docs/research/48 line 67); the header calls
     0xB0..0xB3 clear-on-read, the driver clears them by write-back.
   - fable-4 F6: plausibility: bound AvgCurrent, compare DevName with an expected constant (0x6200 observed), clamp RepSOC slightly over 100 %.
   - fable-4 F7 / fable-5 F5: stale text and dead code: 0148 prose "allowlist {0x36}", board.json notes.fg "0x69 NEVER addressed",
     `f_addr_mask < 0x69` magnitude compare, unused `SFG_DW_NACK_STATE` / `SFG_DW_LOSE`, unused ADT props `st_no_ack` / `st_lose`, 0153 prose.
   - fable-4 F8: `__atomic_store_n` for the sample fields (formally a data race now; fine on arm64).
   - fable-4 F10: CNFG_12 (0xC5, input select) and CNFG_18 (0xCB, watchdog enable) as extra read-only diagnostics (own review).
6. **Infra.** Commits are local on `wip/oriole-next` (not pushed, no CI build of the C4-C6 kernels). The host firewall drops UDP 5140 when active:
   runs use `SWEET_USBLOG_PORT=64738` (patch 0152); a permanent fix is 5140 in `networking.firewall.allowedUDPPorts` (root).

## Kernels and images

- xnu-local-0153 (d6714782): C5. xnu-local-c6b (a1a9846d): C5 + C6 + LOSE/NO_ACK fixes + stale keep; daily image 2 uses it.
- boot_b undo: `fastboot flash boot_b ~/pixelios-artifacts/boot-backup/boot_b-orig.img`.
