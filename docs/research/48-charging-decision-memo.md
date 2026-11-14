# C5: charger writes from XNU — decision memo

Research only, no hardware touched. Date 2026-11-14. No write sequence is designed here.

Tags: **[HW]** = observed on hardware, **[SRC]** = read from source (file:line at the named commit), **[GUESS]** = guess.

Sources used:
- `bms` = kernel/google-modules/bms, branch android-gs-raviole-6.1-android16, `183509a`
- `tcpm` = kernel/google-modules/soc/gs `2040787`, `drivers/usb/typec/tcpm/google/`
- `dts-gs101` = devices/google/gs101 `a9b5edb`; `dts-rav` = devices/google/raviole `81506b9`
- `ref` = stock OrangeFox-recovery capture `~/pixelios-artifacts/ref/oriole/20261114T072643Z/` (dmesg.clean, power-supply.txt), 2026-11-14

## Parts on the bus

**[HW]** All four sit on `hsi2c_12` = 0x10D50000:
- `ref` dmesg:197 `/hsi2c@10D50000/max77759tcpc@25`
- `ref` dmesg:2647 `…/10d50000.hsi2c/i2c-12/12-0036/power_supply/maxfg`
- `ref` dmesg:2395 `pca9468 12-0057`

| Address | Part | DT (**[SRC]**) | Linux driver (**[SRC]**) |
|---|---|---|---|
| 0x69 | MAX77759 charger | `gs101-raviole-battery.dtsi:208-230`, `maxim,max77759chrg`, `chg-term-voltage = 4400` | `bms/max77759_charger.c` |
| 0x36 | MAX77759 fuel gauge (M5) | `gs101-raviole-battery.dtsi:182-206`, `maxim,max77759`, `gauge-type = 2`, `rsense-default = 500` | `bms/max1720x_battery.c` |
| 0x25 | MAX77759 TCPC | `gs101-common-typec.dtsi:103-110` | `tcpm/tcpci_max77759.c` |
| 0x57 | PCA9468 direct charger | `dts-rav/gs101-raviole-charging.dtsi:101-103`; oriole `input-current-limit = 2300000` (`gs101-oriole-charging.dtsi:10-12`) | `bms/pca9468*` |

There is also `max77729pmic@66` on the same bus (`gs101-raviole-battery.dtsi:150-162`). **[SRC]**

## What Linux writes at boot

**Charger 0x69** (`max77759_charger_probe`, `max77759_charger.c:3203-3420`). **[SRC]** register addresses are from `max77759_A1_regs.h`, which `max77759_regs.h:11` includes.
1. Reads `CHG_CNFG_00` (0xB9) as a ping (`:3221`).
2. Watchdog: `wden = false` (`:3235`). `max77759_wdt_enable()` clears WDTEN in `CHG_CNFG_18` (0xCB) and reads it back, because "this register is protected" (`:433-455`, `:3277`).
   - With WDTEN off, the AP does not have to kick the watchdog: no `WDTCLR` in `CHG_CNFG_00` (`:2337`, `:2354`).
3. `CHG_CNFG_01` (0xBA): clears FCHGTIME, which disables the fast-charge safety timer (`:3281-3284`).
4. `CHG_CNFG_13` (0xC6): turns THM2 monitoring off unless the DT asks for it (`:3286-3298`). Oriole has neither property. **[GUESS]**
5. Unmasks interrupts by writing `CHG_INT_MASK` (0xB2) (`:3401-3403`).
6. Charge mode: `max77759_mode_callback` writes `CHG_CNFG_00`, driven by the `CHARGER_MODE` votable (`:1020`, `:1178`; `max77759_chg_mode_write` at `:282`). The modes are:

   | Mode | Value |
   |---|---|
   | ALL_OFF | 0x00 |
   | BUCK_ON | 0x04 |
   | CHGR_BUCK_ON | 0x05 |
   | BOOST/OTG | 0x08-0x0F |

   (`max77759_regs.h:53-62`.) The datasheet table puts the reset value of `CHG_CNFG_00` at 0x04 (`max77759_A1_regs.h:1629`).
   - Protected registers are unlocked through `CHGPROT` in `CHG_CNFG_06` (0xBF, bits [3:2]) (`:300-322`; `max77759_A1_regs.h:1862-1875`).
   - The current and voltage limits (`CHG_CNFG_02` 0xBB, `_04` 0xBD, `_09` 0xC2, `_10` 0xC3, …) are set later, by google_charger votes. That path was not traced line by line. **[GUESS]**
7. Interrupts: the IRQ handler reads `CHG_INT`/`CHG_INT2` (0xB0/0xB1), then **writes the same bytes back** to clear them (`:2961-2980`).
   - **[GUESS]** Write-1-to-clear rather than clear-on-read, judging by the driver. The register header gives no access type.

**Fuel gauge 0x36** (`max1720x_battery.c`). Register map at `max1720x.h:20-76`. **[SRC]**

| Register | Address | Conversion (**[SRC]**, `maxfg_common.h` / `max1720x_battery.c`) |
|---|---|---|
| Status | 0x00 | POR bit; the driver writes back `fg_status_clr` to clear (`max1720x_battery.c:3034`, `:3142`) |
| RepSOC | 0x06 | % = value >> 8 (LSB 1/256 %) (`maxfg_common.h:210-214`) |
| Temp | 0x08 | 0.1 °C = s16 × 10 / 256 (`maxfg_common.h:198-202`) |
| VCell | 0x09 | µV = value × 78.125 (`maxfg_common.h:192-196`) |
| Current / AvgCurrent | 0x0A / 0x0B | µA = s16 × 156250 / RSense, with RSense in 10 µΩ units (`max1720x_battery.c:473-477`); DT RSense = 500, so 5 mΩ |
| FullCapRep | 0x35 | µAh = s16 × 500000 / RSense × lsb (`maxfg_common.h:179-183`) |
| DevName | 0x21 | **[HW]** 0x6200 (`ref` dmesg:1407) |

- The tag map for these registers is at `max1720x.h:301-320`. **[SRC]**
- **Clear-on-read registers:** none found. A grep of `bms` for clear-on-read wording found nothing, and the only status registers that get cleared (FG Status, charger CHG_INT) are cleared by an explicit write-back. **[SRC]**
  - **[GUESS]** The datasheet has to confirm this before a reader treats any register as side-effect free. Until then, a read-only monitor should touch only 0x06, 0x08, 0x09, 0x0A, 0x0B, 0x35 and 0x21.
- **Writes at boot:**
  - **[HW]** "Device 0x6200 has no permanent storage" (`ref` dmesg:1409).
  - **[SRC]** If Status.POR is set, the driver requests a model reload, which writes the M5 model (`max1720x_battery.c:2332-2365`, `808-831`; `max_m5.c`).
  - **[SRC]** Write-backs of Status and the filter config (`maxim,filtercfg-*` in the DT).

**TCPC 0x25** **[SRC]**
- `TCPC_POWER_CTRL`: voltage monitoring, alarms and bleed discharge (`tcpci_max77759.c:832-838`).
- The USB switch (`TCPC_VENDOR_USBSW_CTRL`, `:1104-1158`).
- It votes on `CHARGER_MODE` (`:66`, `:1207`).

**PCA9468 0x57** **[HW]** It sets the input current and the float voltage at probe: "iin=2350000 real iin_cfg=2400000", "v_float=5000000" (`ref` dmesg:2395, 2409).

## Stock state (recovery, phone on a USB cable) **[HW]**

- **`CHG_CNFG_00` during boot** (21 lines): 5→5 (mode callbacks, dmesg:2406-2728) → 5→0 (dmesg:2767) → 0→4 (use_case 0→1, dmesg:2845) → 4→0 → 0→0 …
- **At the end** it is **0x05 (CHGR_BUCK_ON), use_case 1** (dmesg:5237-5355, last at 38.1 s).
- Voter flags along the way:
  - "chgr_on=1 buck_on=1", then "CHGIN_SUSP … chgin_off=1" (dmesg:2842, 2875-2889);
  - "MSC_CHG disable_charging -1 -> 0" (dmesg:2877).
- `power_supply` at capture time:

  | Supply | Values |
  |---|---|
  | `battery` | 68 %, 4,011,875 µV, 33.6 °C, STATUS "Not charging", CHARGE_FULL 4,242,000 µAh (design 4,524,000), 153 cycles |
  | `main-charger` | "Charging", 473,500 µA, CURRENT_MAX 500,000 µA |
  | `usb` | SDP, 4,875,000 µV, 500 mA input limit |
  | `maxfg` | 65 %, CURRENT_NOW −132,500 µA (discharging) |
  | `pca9468-mains` | ONLINE 0 |
  | `gcpm` | CONSTANT_CHARGE_VOLTAGE_MAX 4,200,000 µV |

- **[HW]** PORT.md C0: the phone gains voltage in fastboot on this cable (+26 mV / 10 min) and drains about 2-2.5 mV/min in an XNU GUI run. So the bootloader leaves the charger in a mode that charges.
- **[GUESS]** Under XNU it stays in whatever mode ABL or the TCPC defaults left. The drain is load, not a disabled charger.

## What charging control from XNU would need

1. **Read-only first** (low risk): an I2C master on hsi2c_12, plus FG reads of 0x06, 0x08, 0x09, 0x0B and 0x35 for a battery indicator and thermal gating. Read the charger registers `CHG_DETAILS_00..03` (0xB5-0xB8) and `CHG_CNFG_00`. No writes.
2. **Full control** (high risk) would re-implement:
   - the `CHARGER_MODE` voting (google_charger, google_cpm, the max77759 usecase state machine `max77759_usecase.c`);
   - CHGPROT handling;
   - the watchdog policy;
   - the JEITA/thermal charge profile from the battery DT (`google_battery`);
   - the TCPC/PD negotiation for anything above 5 V;
   - PCA9468 direct charging.
   - It also has to cover OTG and wireless (p9221) interactions.

## Risk

- A wrong `CHG_CNFG_00`/`02`/`04` value can overcharge. The DT limits are 4.4 V termination (`max77759,chg-term-voltage = 4400`), but `gcpm` shows 4.2 V in use. **[SRC]** for 4400; **[HW]** for 4.2 V.
  - Other failure modes: charging at the wrong temperature, setting boost/OTG while a charger is attached, or browning out VSYS if the buck is turned off.
  - The battery is the user's real battery (153 cycles). **[HW]**
- Linux turns **off** the hardware fast-charge timer and the charger watchdog (`:3235`, `:3281`). **[SRC]** So a buggy driver that stops running gets no independent cut-off from those two. **[GUESS]** The remaining protection is in the charger's own hardware limits and the battery pack protection.
- Writes to the FG model area can corrupt SOC tracking (the gauge has no permanent storage, so a POR reload is needed). The risk is wrong readings, not a hazard. **[GUESS]**

## Recommendation

- **No charger, TCPC or PCA9468 writes from XNU.** Keep relying on the bootloader's charge mode (C0 shows the battery holds within budget) and on recovery or fastboot pauses for charging.
- **Allow:** a read-only FG and charger-status reader (the registers listed above), reviewed under rule 8, as the basis for a battery percentage in the UI and for an over-temperature stop. The H3 rule stops at 45.0 °C.
- **Revisit** control only if long runs need it, and then only with a "stop charging" or "input suspend" write (a single bit) after reading the datasheet. Never voltage or current programming.
