VERDICT: SAFE

# Review 4: C5 (36f9d9f, battery % publish) and C6 (6077698, read-only charger status at 0x69)

Reviewer: Claude Fable 5.2 (delegate, worked alone, read-only apart from this file). Date 2026-11-15.
Scope: `git show 36f9d9f 6077698`. P = line of `xnu/patches/0148-sweet-hsi2c-fg.patch` at 6077698, B = line of
`xnu/patches/0153-sweet-batt-publish.patch`, T = `shim/hsi2c_fg_xnu_host_test.c`, A = `tools/test_hsi2c_fg_audit.py`,
BMS = `~/.hermes/cache/scratch/src-gm-bms` at 183509a. Reviews 1-3 still stand for the unchanged code.
No phone access, no kernel built, nothing pushed. Claims are tagged *read from source*, *observed* (host run by me) or *guess*.

SAFE means: for the phone's hardware. With `sweetbatt=2` the only new bus traffic is eight combined transfers to 0x69, each one
allowlisted pointer byte followed by a one-byte read; no data byte can reach the charger, no other device can be started, and a
charger failure stops the reader exactly like a gauge failure. Nothing changes a charger or battery-protection setting
(docs/SAFETY.md). SAFE does not cover what the indicator shows after the reader stops (finding 1), and the "no read side effect"
claim rests on the driver source, not on the datasheet (finding 5).

## Findings

| # | sev | finding | suggested fix |
|---|---|---|---|
| 1 | M (what the user sees, not phone safety) | Once the reader stops, or one set is implausible, `sweet_batt_sample` returns -1 (P:269, P:276) and SweetBattery publishes the static fallback: 100 %, external power, full (B:100-102). The stop is latched for the boot, so after any single bus failure the indicator shows 100 % on external power while the phone is discharging at about 190 mA (c5-pub1), with no low-battery warning. `sweetbatt=2` makes a stop more likely: 16 more messages per set, and a charger failure also ends the gauge reads. | Keep publishing the last valid sample when the reader stops or a set is implausible (flag it stale, e.g. `SweetBatteryFallback`), and use 100 % only when no sample ever existed. Until then, treat `sweetbatt=2` as a diagnostic boot, not the default. |
| 2 | L | External / charging = AvgCurrent > +50 mA (B:106) is wrong in the state being investigated: on USB and discharging it publishes `external=0 charging=0` (*observed* in the c5 log). On a charger at full (current near 0) it says not external and not full at 100 %. No hysteresis, and AvgCurrent is a slow average, so the state lags a plug event and can flip between two 30 s samples near +50 mA. | Fine for an indicator, as the patch text declares. Do not use it for any decision. Once the C6 bytes have been seen on the phone, CHGIN_OK (INT_OK bit 6, BMS max77759_A1_regs.h:1381) is the proper source, as a separate reviewed step. |
| 3 | L | The audit does not pin the new C5/C6 properties outside the four re-pinned functions. My mutants (*observed*): in the glue, "charger reads with sweetbatt=1" (`sb.st.chg = 1`), "sweet_batt_sample calls sfg_set" (a second thread on the bus), "sample ignores stopped" and "any sweetbatt value" all pass `check_glue`, and the glue has no host test. In the header, "charger failure ignored", "TX takes idx", "publish without plausibility", "publish before the restore is verified", "2-byte loop for the charger", "DevName latch dropped" pass the audit and are caught only by the host test; "sfg_sample_set reads a register" passes both. | Pin `sweet_batt_sample` and `sfg_sample_set` by hash, require exactly one `sfg_set(` in the glue (in `sb_thread`), pin `sb.st.chg=on==2;` and `if(on!=1&&on!=2)`, and add the C5/C6 helpers to a no-bus list like `F5_NOBUS`. |
| 4 | L | The eight-register list is pinned as two fragments (A:150), with a gap at the line continuation: a ninth entry inserted after `X(CNFG_00, 0xB9u)` passes the audit. It is stopped only at compile time by `_Static_assert(SFG_NCREGS == 8)` (*observed*: the host test does not build). | Pin the whole `SFG_CREGS` text as `REGS_TEXT` does for the gauge. |
| 5 | L | "No read side effect" is *read from source*, not from the datasheet. Evidence: the eight registers are read singly and repeatedly by BMS (INT_OK :2073/:2100/:2926, DETAILS_00 :1787, DETAILS_01 :1240/:2137, DETAILS_02 :1806/:2086, DETAILS_03 :2708, CNFG_00 :1044, CNFG_02 :1460, CNFG_09 :1518); the sysfs dump bulk-reads all of 0xB0..0xCC (:2841); the regmap marks the whole range readable (:2904-2916) with no precious register; the interrupt bytes are cleared by an explicit write-back (:2961-2980). docs/research/48 line 67 said the datasheet must confirm this; that is still open. The header comment calls 0xB0..0xB3 "clear-on-read" (P:332), which the source does not show (write-1-to-clear by the driver; 0xB2/0xB3 are masks). The exclusion itself is right and conservative. | None for the run. On the first log: two consecutive `SWEET CHARGER: raw` lines must agree apart from real state changes. Correct the comment wording. |
| 6 | L | Plausibility (P:1259-1262) bounds soc, VCell and Temp only. AvgCurrent, which alone drives external/charging, has no bound. DevName is latched from the first plausible set (P:1277), not compared with an expected constant, so a corrupt first DevName blocks every later good set (fails closed, into finding 1). RepSOC slightly above 100.00 % is treated as implausible instead of clamped. | Bound the current (e.g. within +-6 A), compare DevName with the value seen on the phone, clamp RepSOC up to about 105 %. |
| 7 | L | Stale text now contradicts the code: the 0148 patch prose (P:1-10: "Device allowlist {0x36}", "sweetbatt=1", "UNRUN"), the header top comment (P:306-309 "The device is always SFG_DEV_ADDR7"), the allowlist comment (P:776 "MASTER_RUN needs ADDR = 0x36 << 10"), and board.json notes.fg ("0x69 MAX77759 charger ... NEVER addressed"). Also `sfg_cfg_load` checks `f_addr_mask < SFG_CHG_ADDR7` (P:560), a magnitude compare; the intent is `(f_addr_mask & 0x69) == 0x69` (the real mask is 0x3FF, so no effect today). | Update with the next edit of 0148 and board.json. |
| 8 | info | The publish path uses plain stores between `__atomic_thread_fence` calls (P:1290-1298), formally a data race in C. On arm64 with aligned 32-bit words it behaves as intended (see requirement 4). | `__atomic_store_n` for `valid`, `seq` and `stopped` would make it exact. |
| 9 | info | The handoff state is ADDR = 0x9400 (0x25, the TCPC) with AUTO_CONF = 0x00030001, which now equals `v_auto_rc`. The only thing between that state and a one-byte TCPC read is the term `addr == c->v_addr_c` (P:793). It is hash-pinned, has an audit mutant (A:247) and a host case (T: `sfg_w32(&r, 0x44, 0x00030001 | RUN) == -2` with ADDR 0x9400). | None. Keep that case. |
| 10 | info | For the diagnosis itself, the set lacks CNFG_12 (0xC5, CHGINSEL, BMS regs :2104) and CNFG_18 (0xCB, WDTEN, :2389). A charger watchdog left enabled, or the input not selected, are both candidate causes; `chg_dtls` and `mode` may already show them. | Not a defect. Adding registers needs its own review. |

I could not re-read the vendor device tree cite for the address 0x69 (`gs101-raviole-battery.dtsi:208-211`): that tree is not on
this machine. If the address were wrong, the first charger transfer would NACK and stop the reader; 0x69 is none of the excluded
devices.

## Requirement 1: writes to 0x69, the allowlist, other addresses — CONFIRMED

- *Read from source*: every store is `sfg_w32` -> `sfg_w32_allowed` (P:803-817). TX_DATA is allowed only when the live ADDR, read
  just before, is `0x36 << 10` with one of the seven gauge pointers, or `0x69 << 10` with one of the eight charger pointers
  (P:797). The two pointer sets are disjoint. The only write message is `v_auto_w` = length 1 without STOP, so exactly one byte
  leaves per write message; a second byte (register data) cannot be transmitted to either device.
- MASTER_RUN needs AUTO_CONF to already hold the same value without RUN, and: length-1 write towards 0x36 or 0x69, 2-byte read
  only with ADDR = 0x36, 1-byte read only with ADDR = 0x69 (P:791-793). ADDR itself accepts only those two values or the snapshot
  (0x9400, restore, never with RUN).
- *Observed*: my sweep of `sfg_w32_allowed` with the oriole ADT values, 1 605 632 cases, 0 mismatches against that specification:
  all 2^20 low ADDR values; MASTER_RUN for all 1024 master addresses x lengths 0..3 x READ_WRITE x STOP x (config present or not);
  TX_DATA for all 1024 addresses x values 0..0x1FF. No address other than 0x36 and 0x69 gets MASTER_RUN, so 0x66, 0x25, 0x57 and
  0x35 cannot be started. 0xB0..0xB3 are refused as TX bytes; `SFG_CRANGE` rejects them at compile time (P:339).
- After a gauge failure the charger loop does not run (`e == SFG_OK` in its condition, P:1373), so a stale TX byte cannot reach 0x69.

## Requirement 2: the eight registers — CONFIRMED from source, datasheet open (finding 5)

Addresses and fields match BMS max77759_A1_regs.h: INT_OK 0xB4 (:1379), DETAILS_00..03 0xB5-0xB8 (:1454, :1505, :1543, :1588),
CNFG_00 0xB9 (:1632), CNFG_02 0xBB (:1729), CNFG_09 0xC2 (:2024); decoded fields CHGIN_DTLS [6:5], BAT_DTLS [6:4], CHG_DTLS [3:0],
CHGIN_STS [5], MODE [3:0], CHGCC [5:0], CHGIN_ILIM [6:0] as in the header. The wire format (one pointer byte, repeated start, one
byte) is what regmap does for reg_bits 8 / val_bits 8 (:2909-2913). 0xB0..0xB3 are never read. A read does not service the
charger watchdog (that is a write), so the reads do not change charger behaviour.

## Requirement 3: charger failure — PASS

*Read from source*: the charger loop shares `e` with the gauge loop; the restore is unconditional (P:1384); `e != SFG_OK` prints
the diagnostics and `refused CHG <name> reg=0x.. nack|timeout|error restore=...`, sets `stopped`, returns 1 (P:1406-1418); the
thread exits (P:185-188); `sfg_set` returns at once when stopped (P:1324). *Observed*: failure injected at each of the 16 charger
messages, three ways (NACK, TRANS_ABORT, RX timeout / NACK): every case returns 1, stops, ends with the six restore writes, has
zero fake violations, publishes no sample from that set, prints no charger line, and the next call makes no read and no write.

## Requirement 4: the C5 publish path — PASS, with findings 1, 2, 6, 8

- Concurrency: one writer (the reader thread). It sets `valid = 0`, fence, four fields, fence, `valid`, then `seq`. The consumer
  takes two copies with a fence between and accepts only if seq is nonzero and equal, both valid, and all four fields equal. If the
  second copy saw the old `valid`, the whole first copy predates the update (old, consistent). If it saw the new `valid`, the second
  copy is entirely new and the first must equal it. A mixed sample needs two updates inside one call; updates are 30 s apart.
- `sweet_batt_sample` touches memory only; it returns -1 when not armed (`sb.i2c == NULL`), stopped, no set yet (seq 0) or invalid.
- The sample is stored only after a verified restore (P:1425); an implausible or changed-DevName set is logged, not published
  (host cases at T, C5 block). Units: soc 0.01 %, mV, mA truncated, 0.1 C; SweetBattery rounds the percent and caps at 100.
- Fallback when no sample exists: 100 %, never 0 % (so iOS does not shut down at boot). Correct before the first set; misleading
  after a stop (finding 1).

## Requirement 5: the audit — no rule weaker than C6 needs; new properties under-pinned (findings 3, 4)

- Re-pinned: `sfg_w32_allowed`, `sfg_w32`, `sfg_derive`, `sfg_msg_start`, exactly the functions C6 changed. `sfg_reg_writable`,
  `sfg_rd_ok`, `sfg_restore` keep their old hashes. `W32_SITES` unchanged (no new write site). Loops +3, each fixed-count.
- Rules that had to change: "allowlist exactly {0x36}" became {0x36, 0x69} with the never-list asserted for both; the `sfg_xfer`
  check went from the parameter type to a pinned prefix that includes both table look-ups (stricter text); the fake accepts
  MASTER_RUN for the two addresses and ties the read length and the pointer set to the address.
- The six C6 mutants and the reworked "data byte from the caller" mutant are killed (*observed*, 5 tests OK).

## Requirement 6: host runs (*observed*)

- `../pixos-run.sh "bash .../scratchpad/fght/run.sh"` -> `PASS hsi2c_fg_xnu host test`, `exit 0`; the ASan/UBSan build also PASS.
- `../pixos-run.sh "python3 -I tools/test_hsi2c_fg_audit.py"` -> `Ran 5 tests`, `OK`.
- My sweep, failure injection and mutants were built from private copies in the scratchpad (`rev4/`); the tree was not modified.

## What to check on the first sweetbatt=2 log

1. `SWEET CHARGER: raw B4=.. B5=.. B6=.. B7=.. B8=.. B9=.. BB=.. C2=..` every 30 s, after the `src=fg` line, with `restore=ok` on the first set.
2. Two consecutive raw lines agree unless the cable state changed (finding 5).
3. No `refused`, `fail`, `BUS NOT IDLE` or `RESTORE FAILED`. After any of them the indicator shows 100 % (finding 1); power-cycle as before.
