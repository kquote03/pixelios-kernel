# B5: Bluetooth transport, from the iOS 16.7 side and the Pixel 6 (Linux) side

Research only, no hardware touched. Date 2026-11-14.

Tags: **[HW]** = observed on hardware, **[SRC]** = read from source or files (file:line at the named commit, or a named file in an image), **[GUESS]** = guess.

Sources used:
- `ios-root` = `~/pixelios-artifacts/ios-root-oriole-v2.img` (HFSX, volume `iOSRoot`).
  - Listed read-only with `7z l -slt`.
  - Binaries read with sleuthkit `icat`: inode 59898 = `/usr/sbin/BlueTool` (9,760,944 B), inode 60004 = `/usr/sbin/bluetoothd` (8,948,576 B). 7z extracted them as 0 bytes.
  - Only `strings` and plist keys are cited below: short identifiers, no code.
- `nitrous` = kernel/google-modules/bluetooth/broadcom, branch android-gs-raviole-6.1-android16, `d7bd4dd`
- `soc-gs` = kernel/google-modules/soc/gs, same branch, `2040787`
- `dts-gs101` = devices/google/gs101 `a9b5edb` (android-16.0.0_r0.10), under `dts/`
- `dts-rav` = devices/google/raviole `81506b9`
- `linux` = torvalds/linux v6.18: `drivers/bluetooth/btbcm.c`, `btbcm.h`, `hci_bcm.c`
- `ref` = stock recovery capture `~/pixelios-artifacts/ref/oriole/20261114T072643Z/`, 2026-11-14
- `bt-fw` = `~/pixelios-artifacts/bt-fw/`, copied from vendor.img (PORT.md B0)

## 1. What iOS 16.7 expects

### 1.1 Processes and launchd

- **[SRC]** `ios-root` `/System/Library/LaunchDaemons/com.apple.bluetoothd.plist`:
  - `ProgramArguments` = `/usr/sbin/bluetoothd`, `UserName` = `mobile`, `RunAtLoad`, `KeepAlive.SuccessfulExit` = false;
  - it vends `com.apple.server.bluetooth` and related Mach services;
  - its `LaunchEvents` wait on `com.apple.mobile.lockdown.activation_state`.
- **[SRC]** `com.apple.BlueTool.plist`: `/usr/sbin/BlueTool -R`, on demand through the Mach service `com.apple.BlueTool`, user `mobile`.
- **[SRC, bluetoothd strings]** bluetoothd runs BlueTool scripts: `com.apple.bluetooth.bluetool.script`, "BlueTool finished running %{public}s script", "BlueTool timed out running …".
- Other daemons present: `/usr/sbin/BTLEServer`, `BTAvrcp`, `BTMap`, `BTPbap`, `/usr/libexec/bluetoothuserd`. **[SRC]**
- **[SRC]** The rootfs has **no** `/etc/bluetool/` directory and no `.hcd` files (searched the 321,371 listed paths). BlueTool **embeds** its scripts and the patchram images. See 1.4.
- **[SRC]** `/System/Library/Extensions` on the rootfs has no Bluetooth or serial kext (only 17 entries, such as IOHIDFamily and IOUSBHostFamily).
  - **[GUESS]** AppleBluetoothModule, AppleConvergedIPC and the UART driver live in the kernelcache. They are not in this image.

### 1.2 Transports

**UART, H4.** **[SRC, strings]**
- bluetoothd opens `/dev/cu.bluetooth`, falling back to `/dev/uart.bluetooth`: "No valid serial port found (tried cu.bluetooth and uart.bluetooth)".
- The framing is H4: "read() H4_HCI_EVENT header", "FATAL: Unrecognized H4 PTI", `BTUART_H4_RX_STATE_*`, "H4 wake", "H4 sleep", "H4/AP sleep now", and "H4DS" (the H4 deep-sleep state machine).
- Nothing points to H5 (three-wire) as the transport.
  - "Sleep Mode, (0x09 = H5)" appears only in the help text of `bcm -s`. That is the Broadcom LPM mode byte, not the HCI framing.
- **[GUESS]** H4 with HW flow control, as on every Apple UART BT part.

**UART over Skywalk.** **[SRC]**
- bluetoothd has `com.apple.uart.sk.bluetooth`, "BM3UART read/write failed", `os_channel_create` and `IOAOSSKYSETCHANNELSPEC`. That is a Skywalk channel to a UART nexus.
- **[GUESS]** Newer SoCs use it; the plain tty node is the fallback.

**PCIe (newer parts, not usable on Pixel 6).** **[SRC]**
- `AppleConvergedIPC`, `AppleConvergedIPCOLYBTControl` ("doorbell flush"), `AppleConvergedTransport.dylib` (`…Create/Read/Write/SendImage`), `skywalk_read_channel`, `pcie_flr`, and "PCIe could not find AppleBluetoothModule service".

**Apple ACI / "Marconi" controller.** **[SRC]**
- `AppleBTHci`, `AppleBTMgmt`, their user clients `AppleBTHciUC` and `AppleBTMgmtUC`, and `aci_*_init.hcd`.
- These are Apple's own BT chips. They do not apply to Pixel 6.

### 1.3 IOKit names and other inputs

**IOKit class names seen in strings.** **[SRC]**
- `AppleBluetoothModule` and `AppleBluetoothModuleUserClient`
- `AppleConvergedIPC`, `AppleConvergedIPCInterface`, `AppleConvergedIPCUserClient0`, `AppleConvergedIPCControl(UserClient)`, `AppleConvergedIPCSkywalkInterface`
- `AppleBCMWLANBusInterfacePCIe`
- `IOUserBluetoothSerialDriver`
- BlueTool also uses `IOServiceFirstMatch` and `IORegistryEntryFromPath("IODeviceTree:/…")`.

**Power, reset and wake control.** **[SRC, BlueTool strings]**
- Device files: `/dev/btpoweroff`, `/dev/btreset`, `/dev/btwake`.
- Commands and functions: `power on|off`, `reset on|off|pulse`, `wake on|off|pulse`, `command_regon_power_on`, `command_regon_power_off`.
- bluetoothd also opens `/dev/btwake`: "Unable to turn on BT)_WAKE signal".
- **[GUESS]** These are character devices published by the kernel BT-module driver (AppleBluetoothModule). A PixOS port has to provide them, or patch the scripts.

**Device-tree properties BlueTool reads.** **[SRC]**
- Node `bluetooth` (or `marconi-bt`), with `transport-speed`, `transport-encoding` and `local-mac-address`.
- Calibration blobs: `bluetooth-tx-calibration`, `bluetooth-rx-calibration`, and others.
- Environment overrides: `BT_BAUDRATE`, `BT_DEVICE_ADDRESS`, `BT_UART`, `BT_CHIP`.
- Boot-args read through `kern.bootargs`: `wifibt-external` (values `0x442514e4`, `0x443314e4`, `0x443414e4`; **[GUESS]** PCI device:vendor ids, 0x14e4 = Broadcom), `wlan.factory`, `wlan.debug.generate-mac`.
- **[SRC]** BlueTool `device -S`: "Configure the device for using the speed/baudrate from the device tree". `device -d <device>`: "Open the specified UART device". The default UART name is `com.apple.uart.bluetooth`.

### 1.4 Boot scripts and patchram

**Script source.** **[SRC]**
- BlueTool looks for `/etc/bluetool/%s.boot.script` and runs the command `bcm -w <file.hcd>` from it.
- When no file is present it uses built-in sequences ("Using default init file", "No boot script for the platform").

**Embedded Broadcom command sequence.** **[SRC]** The strings run in this order. **[GUESS]** That this is one script, and that it runs in this order:

```
wake on ; msleep 100 ; hci reset ; bcm -B ; msleep 200 ; bcm -W ; device -s 115200 ;
bcm -A ; bcm -N ; bcm -Q ; bcm -s 0x01,... ; msleep 200 ; device -D -S ; wake off ;
bcm -b 1500000 ; bcm -x ; bcm -s ...
```

What the commands mean, from the help text **[SRC]**:

| Command | Meaning |
|---|---|
| `-B` | set the baud rate using a predefined value |
| `-W` | download the default HCD |
| `-A` | BDADDR from the device tree |
| `-N` | local name = host name |
| `-Q` | load calibration |
| `-s` | sleep-mode parameters |
| `-x` | regulatory power table |
| `-b` | set the baud rate |
| `device -s 115200` | the host goes back to 115200 after the patch, because the controller resets to its ROM baud |

**Embedded patch images.** **[SRC]**
- UART `.hcd` images for BCM4350C5, BCM43452A2, BCM4355C0 and BCM4357B1. Names like `BCM4357B1_19.1.282.xxxx_<Board>_OS_{MUR,USI}_2021….hcd`.
- PCIe `.bin` images for BCM4377B2, BCM4378B1, BCM4387C2 and BCM4388B0/C0.
- There is **no BCM4389 image**. The help also lists the chip names BCM4325, BCM4329B1/C0 and BCM20710.
- **[GUESS]** BlueTool picks the image by chip id and module vendor (MUR = Murata, USI). It will not find a match for the Pixel 6 chip.
  - So PixOS needs either an `/etc/bluetool/<platform>.boot.script` with `bcm -w /path/BCM.hcd`, or a pre-patched controller.
  - **[GUESS]** That BlueTool honours `-w` with an arbitrary path for a chip it does not know.

**Takeaway for PixOS:**
1. **[SRC]** The iOS stack wants an H4 UART published as `/dev/cu.bluetooth` or `/dev/uart.bluetooth`.
2. **[SRC]** It wants `/dev/btpoweroff`, `/dev/btreset` and `/dev/btwake`.
3. **[SRC]** It wants an ADT node `bluetooth` with `transport-speed` and `local-mac-address`.
4. **[GUESS]** A BlueTool-runnable path to a `.hcd`.

The classic Broadcom UART flow BlueTool implements (HCI Reset → minidriver/patchram → baud change) is the same flow as Linux btbcm. See 2.3.

## 2. Pixel 6 (oriole) Bluetooth hardware, from the Linux side

### 2.1 DT and pins

**[SRC]** `dts-gs101/gs101-common-bluetooth.dtsi:13-57`. It is included by `gs101-common.dtsi:9`. Oriole has no override; raven has its own file.

| Signal | Pin | DT property and settings | Stock state in recovery **[HW]** (`ref` pinctrl-pins.txt) |
|---|---|---|---|
| BT_REG_ON | `gpp16-2` | `shutdown-gpios`, active high; output, pull none, 2.5 mA | CON 0x0 (input), DAT 0 (line 56): **BT off** |
| BT_DEV_WAKE | `gpp14-3` | `device-wakeup-gpios`, active high; output; power-down pull down | CON 0x0, DAT 0 (line 51) |
| BT_HOST_WAKE | `gpa6-6` | `host-wakeup-gpios`, active high; function F (EINT), pull down | CON 0x0, DAT 0 (line 195) |
| UART16 | `gpa3-0..3` | function 2 (`gs101-pinctrl.dtsi:121-142`) | CON 0x2 on all four (lines 162-165): gpa3-1 DAT 1, gpa3-2 DAT 1 |

- `compatible = "goog,nitrous"` sits under `/odm/btbcm`. **[HW]** `ref` dt/base/odm/btbcm is present.
- Lines: gpa3-1 is TX (`uart16_bus_tx_dat`) and gpa3-2 is RTS (`uart16_bus_rts`) (`gs101-pinctrl.dtsi:127-142`). **[GUESS]** gpa3-0 is RX and gpa3-3 is CTS, by the usual Exynos order.
- **UART: `serial_16` at 0x175B0000** (`gs101-usi.dtsi:955-977`), `samsung,exynos-uart`. **[SRC]**
  - It is in the APM domain (USI1_UART, `samsung,usi-phandle = <&sysreg_apm>`, offset 0x4), with a 256-byte FIFO and `rts-trig-level = 1`.
  - It has `samsung,rts-gpio-control`. The pinctrl states are "default", "rts" and "tx_dat".
  - Clocks: `MUX_APM_FUNC` and `DOUT_CLK_APM_USI1_UART`.
  - **[HW]** `ref` dmesg:952: "175b0000.serial: ttySAC16 at MMIO 0x175b0000". `bt_vendor.conf` names `UartPort = /dev/ttySAC16` (`bt-fw/bt_vendor.conf`).
- **Flow control** is set by userspace termios. `CRTSCTS` enables the auto flow control bit (`UMCON` AFC) (`soc-gs/drivers/tty/serial/exynos_tty.c:2010-2011`). **[SRC]** **[GUESS]** The Broadcom HAL always sets it.
- **Chip:**
  - **[SRC]** WLAN on oriole is `bcmdhd4389.ko` (`dts-rav/insmod_cfg/init.insmod.oriole.cfg:9`).
  - **[GUESS]** BT is the BT core of the same BCM4389 combo.
  - **[SRC]** `bt_vendor.conf`: `FastDownloadDisable = false`, `BigHammerBtRegOnDelay = 100`, `AccelBtDisable = false`, `AocPowerPinCtrlCheckEnable = false`.

### 2.2 The nitrous driver (power and LPM only, no HCI)

- **[SRC]** `nitrous/nitrous.c`. It only handles rfkill, the REG_ON and wake lines, the host-wake IRQ and runtime PM of the UART. Patchram is done by the userspace HAL (`android.hardware.bluetooth-service.bcmbtlinux`, `bt-fw` B0).
- **Power on, rfkill unblock** (`nitrous.c:615-629`):
  1. REG_ON low;
  2. 30 ms;
  3. REG_ON high;
  4. DEV_WAKE high.
- **Power off** (`:631-639`): DEV_WAKE low, then REG_ON low.
- The GPIOs are requested as: `shutdown` with `GPIOD_OUT_LOW`, `device-wakeup` with `GPIOD_OUT_LOW`, `host-wakeup` with `GPIOD_IN` (`:659`, `:725`, `:729`). `wake-polarity` defaults to 1 when absent from the DT (`:710-713`).
- **LPM:** the host-wake IRQ is on both edges (`:206-207`). TX takes a runtime-PM reference and asserts DEV_WAKE (`:90-133`).
- **[GUESS]** The `BigHammerBtRegOnDelay = 100` in the HAL config is a further REG_ON delay in ms, added on top of the driver's 30 ms.

### 2.3 Broadcom bring-up protocol (Linux btbcm / hci_bcm, v6.18)

All **[SRC]** unless marked.

1. **Open at 115200 8N1.** `bcm_proto.init_speed = 115200` (`hci_bcm.c:1302`), with H4 framing (`hci_bcm.c:684-687`).
2. **HCI_Reset** (0x0C03), then 100 ms (`btbcm.c:274-290`). Then Read Local Version. `lmp_subver` selects the firmware name (`btbcm.c:584-627`).
3. **Patchram** (`btbcm.c:211-270`, called at `:680`):
   1. `0xfc2e` Download Minidriver, no parameters;
   2. 50 ms;
   3. send every record of the `.hcd` as an HCI command (opcode, plen, params) and wait for Command Complete each time;
   4. 250 ms after the last one.
   - The files are `brcm/<hw_name>[postfix][.board].hcd` (`btbcm.c:652-666`).
   - **[SRC, parsed offline]** The Pixel `.hcd` files are pure record streams, each parsed exactly to its end:
     - `BCM.hcd`: 3,038 × `0xfc4c` (Write RAM) and 1 × `0xfc4e` (Launch RAM, last);
     - `BTFW_B.hcd`: 3,033 + 1;
     - `BTFW_D.hcd`: 3,038 + 1.
     - So `0xfc4c` and `0xfc4e` come from the file. btbcm.c does not name them.
4. **After the patch the controller reboots at its ROM baud.**
   - btbcm re-runs `btbcm_initialize`: reset and version (`btbcm.c:696-713`).
   - `bcm_setup` restores the host speed with `init_speed` (`hci_bcm.c:600-609`).
5. **Baud change** (`hci_bcm.c:178-226`):
   - If the speed is above 3,000,000, first send `0xfc45` Write UART Clock Setting with `0x01` = 48 MHz (`btbcm.h:9`; `hci_bcm.c:185-201`).
   - Then `0xfc18` Update Baud Rate with params `{__le16 0, __le32 baud}` (`btbcm.h:12-15`; `hci_bcm.c:206-219`).
   - Then switch the host UART to the new speed (`hci_bcm.c:613-625`).
   - **[SRC]** The ACPI default oper speed is 4,000,000 (`hci_bcm.c:989`). **[GUESS]** The Pixel HAL's speed is unknown (`bt-fw` B0: closed libbt-vendor). Typical is 3 Mbaud, and BlueTool's own script uses 1,500,000.
6. **Optional steps:**
   - `0xfc01` Write BD_ADDR (`btbcm.c:153`);
   - `0xfc27` sleep or LPM parameters (`hci_bcm.c:400`);
   - PCM parameters.

## 3. Recommended B2/B3/B4 sequence for a bare-metal test

This is a proposal. The step names follow on from PORT.md B0/B1. Each step needs its own review (AGENTS rules 7 and 8) and a human at the phone.

**B2: read-only readback** (no writes)
- From XNU, or from the probe shim, read the pinctrl CON/DAT of gpp16-2, gpp14-3, gpa6-6 and gpa3-0..3, and the UART16 ULCON/UCON/UFCON/UMCON/UBRDIV registers at 0x175B0000.
- Expected: the stock recovery values in 2.1 if the bootloader leaves them alone.
- **[GUESS]** The bootloader may leave them in a different state from recovery. Record what is found.
- Pass when the values are recorded and nothing hangs.

**B3: power on and HCI identify** (first writes: GPIO plus UART TX only)
1. Set UART16 to 115200 8N1 with AFC on, and the pins to function 2.
2. Power sequence as nitrous does it: REG_ON low → 30 ms (**[GUESS]** also add the HAL's 100 ms) → REG_ON high → DEV_WAKE high.
3. Wait about 100 ms.
4. **[GUESS]** For the expected reply bytes. Send H4 `01 03 0C 00` (HCI_Reset). Expect the Command Complete event `04 0E 04 01 03 0C 00`.
5. Send `01 01 10 00` (Read Local Version). Record `hci_rev` and `lmp_subver`.
6. Power off: DEV_WAKE low, REG_ON low.

- No vendor commands, no RAM writes. The radio does not transmit after a reset; it needs scan or advertising commands for that.
- **[GUESS]** That there is no RF emission until the host enables inquiry, scan or advertising.
- Pass when the reset and version answers arrive.

**B4: patchram at 115200, then a baud change** (vendor writes to controller RAM, volatile)
1. B3 steps 1-4.
2. `0xfc2e` → 50 ms → replay `BCM.hcd` (or the variant B1 selects) record by record, waiting for each Command Complete → 250 ms.
3. The controller comes back at 115200. Send Reset and Read Local Version, and expect a new `lmp_subver` or build.
4. Optionally `0xfc18` to 1,500,000 or 3,000,000 (with `0xfc45` above 3 M), switch the host, and send Read Local Version again.

- Patchram is lost when REG_ON drops, so everything is reversible by powering off. **[GUESS]**
- Which `.hcd` (BCM / BTFW_B / BTFW_D) matches this unit is still open. **[GUESS]** It follows chip revision B/D. Choose by the B3 version reply.

**After B4:** publish a tty-like IOKit serial node as `uart.bluetooth`, `/dev/btwake`, `/dev/btreset` and `/dev/btpoweroff` shims, and an ADT `bluetooth` node (`transport-speed`, `local-mac-address`). Then try bluetoothd with a pre-patched controller, or with a BlueTool boot script calling `bcm -w`. **[GUESS]**
