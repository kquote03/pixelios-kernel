VERDICT: SAFE

# Review 5: 9d1f154 — 0148 no mid-transfer NO_ACK (0xB) state failure (c6-chg1); 0153 keep the last real values after a reader stop (review 4 F1)

Reviewer: Claude Fable 5.2 (delegate, worked alone, read-only apart from this file). Date 2026-11-15.
Scope: `git show 9d1f154` (3 files). P = line of `xnu/patches/0148-sweet-hsi2c-fg.patch`, B = `xnu/patches/0153-sweet-batt-publish.patch`,
T = `shim/hsi2c_fg_xnu_host_test.c`, LX = Linux v6.18 `drivers/i2c/busses/i2c-exynos5.c`, SGS = the Google gs copy of the same file,
LOG = `logs/runs/20261115T145036Z-c6-chg1/merged.log`. Reviews 1-4 still stand for the unchanged code.
No phone access, no kernel built, nothing pushed. Claims are tagged *read from source*, *observed* (host run or log read by me) or *guess*.

SAFE means: for the phone's hardware. The delta removes one failure condition from `sfg_poll` and adds no write, no read, no new value and
no new transfer. Every bound, the allowlist, the restore and the latched stop are byte-identical to d770c21. A real NACK still stops the
reader, at worst 10 ms later. The 0153 part touches memory and IOKit properties only. SAFE does not cover what the indicator shows after a
stop: it is now a frozen real value instead of 100 %, which is better but still not a live reading (findings 1, 2).

## Findings

| # | sev | finding | suggested fix |
|---|---|---|---|
| 1 | M (what the user sees, not phone safety) | After a reader stop the indicator is frozen for the rest of the boot at the last real percent (B:106-109; the stop is latched, P:1324). The phone keeps discharging (about 190 mA in c5-pub1) with no falling percent and no low-battery warning. This is the fix I asked for in review 4 F1 and it is better than 100 %, but it only changes the wrong number into an old number. | Accept for now. Show the stale state somewhere the user sees it, or let the reader retry a set after a clean `restore=ok` timeout (its own reviewed step). Power-cycle after any stop, as before. |
| 2 | L | The log does not show that a published value is stale. `publish(..., source 2)` prints `SWEET BATTERY: published N% ...` every 30 s (B:86-89) with the frozen values; only the property `SweetBatteryStale` differs. Someone reading a run log sees a live-looking line for as long as the phone runs. The stale publish also sets `SweetBatteryHardwareSampleValid = true` and `SweetBatteryCurrentValid = true` with an old current, and `InstantAmperage` can stay positive while `IsCharging` is false. | Print `published (stale)` (or skip the line when stale) and pass `currentValid = false` for the stale publish. |
| 3 | L | H1 of review 3 is still open, and it now decides how a real NACK is seen. With INT_ENABLE = TRANSFER_DONE only, nothing shows whether INT_STATUS raises NO_DEV_ACK / NO_DEV. c6-chg1 gives no evidence: it was not a NACK (finding 6). If the bits are masked, the only NACK detection left is "no RX byte / no TX done within 10 ms". That is what the SGS polling read does (SGS:895-916) and it fails safe (requirement 2), but the reader then reports `timeout`, never `nack`. | None for the run. Do not read a `timeout` line as "bus stuck"; it may be a plain NACK. |
| 4 | L | The host test does not cover the case the delta now relies on. The fake's NACK always sets NO_DEV_ACK in INT_STATUS (T:160-165), the optimistic side of H1. No case has "state 0xB, INT_STATUS 0, no data". My sweep (below) passes for all 30 messages, with the bus released and with it held. The new case (T:657) also differs from c6-chg1: the fake has both bytes in the RX FIFO at once, while on the phone RX was empty when 0xB was sampled. | Add a silent-NACK injection (state 0xB, no interrupt bit, no data) expecting `timeout`, the restore tail and the latched stop; and a variant of T:657 where the bytes arrive after the 0xB polls. |
| 5 | L | Dead code and stale text after the change: `SFG_DW_NACK_STATE` can no longer be produced, but P:840 still tests it and P:841/P:843 still cite "state NO_ACK :154" / "state LOSE :155"; `st_no_ack` and `st_lose` are still required ADT properties that nothing reads; the 0153 prose still says "once the reader stopped, the static fallback stays" (B:3) next to the new line that says the opposite (B:6), and the same sentence is in the code comment (B:99). | Remove with the next edit of 0148 / 0153. |
| 6 | info | The commit text says "0xB mid-read with the bus still busy" at the restore. LOG:1884-1888 shows more: `restore wait idle=1`, then `after FIFO_STATUS=00020100 TRANS=00080001`. The bus was idle at the restore; `BUS NOT IDLE` came from two bytes in the RX FIFO. So the read of 0x0A completed normally after the reader had given up at 58.6 us (DT=0x5A0 at 24.576 MHz, poll 20). This is the strongest evidence that 0xB was a passing state and not a NACK. The restore failure in c6-chg1 was caused by the reader's own early abort. | None. Worth one line in the run notes. |
| 7 | info | The state names are not verified for gs101. The handoff TRANS state is 1 (LOG:1882): LX calls 0x1 START (LX:144), SGS calls it INIT (SGS:159). SGS treats 0x6 as "arbitration lost before transfer" (SGS:1077-1078), LX uses 0xC (LX:687). Both runs (0xC in c4-batt3, 0xB in c6-chg1) saw "error" states inside good transfers. | Do not add any other master-state compare inside a transfer. The idle / init compare in the preconditions is before a transfer and is consistent with the handoff. |
| 8 | info | One exposure is unchanged but now rests on INT_STATUS alone: a device that NACKs the pointer byte and then ACKs the read would return the register at its old pointer. The NO_ACK state compare was a polled, passing state and did not close this reliably either; SGS polling has the same exposure. The MAX77759 gauge ACKs all seven pointers (seven runs of evidence), and a wrong SOC / VCell / Temp must still pass plausibility and the DevName latch. | None. Review 4 finding 6 (bound AvgCurrent) would narrow it. |

## Requirement 1: what the two drivers do — CONFIRMED, neither fails a transfer on master state NO_ACK

- *Read from source*: `HSI2C_MASTER_ST_NO_ACK` is defined once in each file (LX:154, SGS:174) and used nowhere (grep count 1 and 1).
- LX, EXYNOS7 path (LX:521-541): the decision is INT_STATUS only. TRANS_DONE -> done; TRANS_ABORT -> -EAGAIN; NO_DEV_ACK or NO_DEV ->
  -ENXIO; TIMEOUT -> -ETIMEDOUT. The EXYNOS5 path (LX:543-568) uses the flag bits of TRANS_STATUS (bits 1-4), not the state field.
  The state field is compared only with LOSE and only before a transfer (LX:670-693). LX enables all error interrupts (LX:98-102).
  A transfer that raises nothing ends in the 100 ms timeout and a controller reset (LX:811-827).
- SGS, interrupt mode: any of INT_STATUS bits 8-11 (`HSI2C_INT_CHK_TRANS_STATE`, SGS:98, SGS:733) -> `trans_done = -ENXIO`, stop;
  TRANS_STATUS is read only to print it (SGS:734-736). SGS, polling mode: INT_ENABLE = TRANSFER_DONE only (SGS:883); the read loop looks
  at RX_FIFO_EMPTY and the deadline, nothing else (SGS:895-905); a NACK is "rx timeout" -> register dump and reset (SGS:911-916). The
  write loop needs TRANSFER_DONE and TX_FIFO_EMPTY before the deadline (SGS:971-988). State compares in SGS: `== INIT` after an
  interrupt-mode transfer with clock stretching (SGS:931, :1019) and `== 0x6` before a transfer (SGS:1077-1078).
- So the reader after this delta decides as LX does (INT_STATUS error bits) plus as SGS polling does (RX byte or deadline), and is
  stricter than SGS polling: it still fails on INT_STATUS error bits and on ERR_STATUS when they are visible.

## Requirement 2: a real NACK now, and the restore — fails safe

What a real NACK looks like (*read from source*, P:819-845, P:864-890):

- If INT_STATUS shows NO_DEV_ACK / NO_DEV with only TRANS_DONE enabled: `why=nack-int`, `refused ... nack`, at the next poll. Unchanged.
- If it does not (H1, *guess*): on the pointer write, TX done never comes (or it comes and the read then gets no byte); on the read, the
  RX FIFO stays empty. `sfg_wait` ends at 10 ms of counter or 100000 polls, whichever is first: `refused ... timeout`. Before this
  delta the same NACK stopped the reader a few polls earlier through the state compare, if a poll happened to sample 0xB.
- Then, on both paths: no further transfer in this set (the loops need `e == SFG_OK`), the unconditional restore (P:1384), the
  diagnostics, `stopped = 1`, return 1; `sfg_set` returns at once on the next call (P:1324) and `sb_thread` exits. No sample is stored
  (`sfg_sample_set` is reached only after seven good reads and a verified restore, P:1425).
- *Observed* (my sweep, private copy in the scratchpad `rev5/`, ASan/UBSan build): a silent NACK (state 0xB, INT_STATUS 0, no data, no
  TRANS_DONE) injected at each of the 30 messages of a `sweetbatt=2` set, once with the bus released and once with MASTER_BUSY held:
  60 cases, all return 1 with `timeout`, zero fake violations, the six restore writes last, the five words back at the handoff values,
  `st.last.valid == 0`, no `src=fg` and no charger line, and the next call makes no read and no write. Bus released: `restore wait
  idle=1`, `restore=ok`. Bus held: `restore wait idle=0`, `!!! BUS NOT IDLE`, `restore=FAILED`.
- Cost: up to 10 ms more with preemption off for a real NACK. The worst case per set (10 ms wait + 10 ms idle wait) is the one review 1
  accepted; `SFG_XFER_MS`, `SFG_POLLS`, `sfg_wait`, `sfg_idle_wait`, `sfg_restore`, `sfg_w32_allowed` are byte-identical (*observed*:
  the headers extracted from d770c21 and 9d1f154 differ in the two lines of `sfg_poll` only).

The restore while a transfer may still be active (*read from source*; the controller's reaction is a *guess*, neither driver does this):

- `sfg_idle_wait` first waits up to 10 ms for MASTER_BUSY = 0, reads only (P:1064-1077). A 3-byte transfer takes about 70 us, so after a
  NACK (the controller sends STOP) or a late completion the restore runs on an idle controller. That is what happened in c6-chg1
  (`idle=1`). With this delta the reader no longer aborts 58 us into a good transfer, so "restore right behind a live transfer" is now
  less likely than before, not more.
- If the bus is still busy after that (SCL held, a hung device): the six writes go to a busy controller. They are AUTO_CONF without
  MASTER_RUN, ADDR, CTL, FIFO_CTL, INT_ENABLE and a write-1-to-clear of INT_STATUS. The allowlist is value based and does not depend on
  the bus state: no restore write can start a transfer (`(v & v_run) == 0`), and TX_DATA is never written by the restore. At most one
  byte is ever in the TX FIFO, and it is one of the allowlisted pointer bytes. So the worst case I can construct is a stalled message
  that resumes with the restored ADDR (0x25, the TCPC) or the restored AUTO_CONF (a 1-byte read): one pointer byte with no data byte,
  or a 1-byte read. Neither can change a device register. LX and SGS reset the controller here (LX:823, SGS:912); the reader cannot
  (no reset value is allowlisted) and instead reports `restore=FAILED`, stops, and leaves the controller until a reset. No other code
  in this kernel uses HSI2C12, and the charger runs without the bus.
- The `BUS NOT IDLE` case of c6-chg1 (LOG:1884-1888): the bus was idle, the five words were back, and two unread bytes were left in the
  RX FIFO. Nothing pops them (the reader is stopped, and `sfg_rx_pop` is the only RX read). They are data from the gauge's Current
  register and sit in the controller until it is reset. No write follows, no transfer follows. Safe; and with this delta the same
  sequence ends as a normal read.

## Requirement 3: 0153 part (b) — PASS, with findings 1, 2

- *Read from source*: `fGaugePct / fGaugeMv / fGaugeMa / fGaugeCentiC` are assigned in one place only, after `sweet_batt_sample`
  returned 0 (B:113-119). `sweet_batt_sample` returns 0 only for a set that passed `sfg_plausible` and the DevName latch, was stored
  after a verified restore, and was read twice with equal fields (P:264-284, P:1265-1299, P:1425). The stale branch publishes exactly
  those members and only when `fGaugeSeen` is true, which is set in the same block. The members are zeroed in `init` (B:44-46); with
  `fGaugeSeen == false` the old 100 % fallback is used. So a value that never passed plausibility cannot be published.
- An implausible set gives -1 once and the stale flag; the next good set clears it (`SweetBatteryStale = false`, B:120).
- The stale publish says external = false, charging = false, full = false. It claims nothing the reader did not measure, except that
  the current is old (finding 2).
- No hardware access: the delta adds four members, two `putBool` calls and one `publish` call. `putBool` is `setPSProperty`
  (0061:273-280). The only external symbol of the patch is still `sweet_batt_sample`, which reads memory. `poll()` runs on the
  work loop (and once from `start` before the timer exists), so the new members need no lock.
- Hunk line counts of both patches are consistent (*observed*, counted mechanically: 5 and 9 hunks, 0 bad).

## Requirement 4: host runs (*observed*)

- `../pixos-run.sh "bash .../scratchpad/fght/run.sh"` -> `PASS hsi2c_fg_xnu host test`, `exit 0`; the ASan/UBSan build also PASS.
- `../pixos-run.sh "python3 -I tools/test_hsi2c_fg_audit.py"` -> `Ran 5 tests`, `OK`.
- Test first holds: the new test built against the d770c21 header fails (exit 1) at the new case T:657-658.
- My sweep and the old-header build used private copies in the scratchpad (`rev5/`); the tree was not modified.

## What to check on the next sweetbatt=2 log

1. No `fail at`, `refused`, `BUS NOT IDLE` or `RESTORE FAILED`; `restore=ok` on the first set; a `src=fg` line every 30 s.
2. If a `refused ... timeout` appears: read the `fail` lines. INT_STATUS with bit 9 or 10 set would settle H1 the good way; INT_STATUS 0
   with RX empty at 10 ms is a silent NACK or a stuck bus (finding 3).
3. After any stop, `published` lines continue with the frozen values (finding 2). Do not read them as live. Power-cycle as before.
