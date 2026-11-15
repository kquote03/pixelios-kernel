VERDICT: SAFE

# Review 3: xnu patch 0148 sweet-hsi2c-fg, commit 0b7782b (no mid-transfer LOSE failure, answer to the c4-batt3 `why=lose-state` stop)

Reviewer: Claude Fable 5.2 (delegate, worked alone, read-only apart from this file). Date 2026-11-15.
Scope: `git show 0b7782b`, 2 files: `xnu/patches/0148-sweet-hsi2c-fg.patch` (P = line of that file at 0b7782b, one changed line, P:744)
and `shim/hsi2c_fg_xnu_host_test.c` (T, a fake knob `lose_mid_at` / `lose_left` and one case, T:584-585). Reviews 1 and 2 still stand for
the unchanged code. LX = `src-linux-v6.18/drivers/i2c/busses/i2c-exynos5.c`, SGS = `src-soc-gs/drivers/i2c/busses/i2c-exynos5.c`.
No phone access, no kernel built, nothing pushed. Claims are tagged *read from source*, *observed* (host run by me) or *guess*.

SAFE means: the change removes one comparison on a word the reader already reads. It adds no register read, no write, no wait and no
path around the restore. Every way I found for it to go wrong ends in the existing 10 ms timeout or a failed restore check, and both
stop the reader for the boot. SAFE does not mean the printed numbers are trustworthy: nothing in the code checks them (H2).

## Findings

| id | sev | finding | required fix |
|---|---|---|---|
| H1 | L | "As in Linux" holds for the decision rule, not for the interrupt setup. LX enables every error source (`HSI2C_INT_I2C_TRANS`, LX:98-102), so TRANS_ABORT / NO_DEV / TIMEOUT reach INT_STATUS. The reader writes INT_ENABLE = TRANSFER_DONE only (P:787, the SGS polling branch SGS:883). Whether INT_STATUS on gs101 shows error bits whose enable bit is clear is not known from either source (*guess*: it may not). If it does not, then with the LOSE compare gone a real arbitration loss is seen only as "no RX byte within 10 ms" (timeout) or by ERR_STATUS / the NO_ACK state. That is exactly what the SGS polling read does (SGS:895-916: RX_FIFO_EMPTY and a deadline, no error bit at all), and it fails closed (*observed*, mutant M1 below). | None for the run. Do not describe the reader as detecting arbitration loss by interrupt until a phone log shows an error bit in INT_STATUS with INT_EN=0x80. |
| H2 | M (result quality, not phone safety) | Nothing checks the values. `sfg_battery_line` (P:1093-1110) prints whatever the two bytes were; the host test itself pins `soc=255.99% v=5119 mV` for 0xFFFF (T:505). I2C has no checksum here, and with H1 a corrupted byte that still arrives in the RX FIFO is printed as a reading. The values go to the log only: `raw[]` is local to `sfg_set` (P:1137) and `sb_thread` ignores it (P:184), so no kernel consumer can act on a wrong number. | None in code for a read-only diagnostic run. The run's PASS must rest on the operator checks under "Requirement 4". A range check and the DEVNAME compare become required before any consumer (power source, UI, charging decision) reads these values. |
| H3 | L | The new case does not model what the phone showed. Log lines 1894-1896: step `rd-byte1`, poll 14, INT_STATUS=00000000, FIFO_STATUS=00010100 (RX not empty, level 1: the second byte was already there), TRANS=000E000C (MASTER_BUSY still set). The fake shows 0xC on the first four TRANS reads from `rd-byte0` on, with TRANS_DONE already set, and the comment at T:581-583 says "INT_STATUS has TRANS_DONE", which the log does not show. The case still pins the changed line: it fails on the old code (*observed*). | Correct the comment. Optional: start the 0xC window at the second byte with DONE not yet set. |
| H4 | L | Leftovers: the comment at P:758 still lists "state LOSE :155" as a cause of SFG_ERROR; `SFG_DW_LOSE`, the name "lose-state" (P:629, P:860) and the loaded field `st_lose` (P:360, P:388) are now unused. | Tidy with the next edit of 0148. |
| H5 | L | No committed case for "state 0xC, no TRANS_ABORT bit, no data". `abort_at` (T:159) sets the interrupt bit and the state together, and `int_errs` was tested before LOSE in the old order too, so that case never depended on the removed compare. | Add the M1 case (expect `timeout`, reader stopped). |

## Requirement 1: the claim against both sources — CONFIRMED

- LX (*read from source*): the IRQ handler for EXYNOS7 / EXYNOS8895 / AUTOV9 decides on INT_STATUS only (LX:521-541: TRANS_DONE, TRANS_ABORT
  -> -EAGAIN, NO_DEV_ACK / NO_DEV -> -ENXIO, TIMEOUT -> -ETIMEDOUT). It does not read TRANS_STATUS on that path; only the old EXYNOS5
  variant does (LX:543-563), and there for flag bits 0-4, not the state field. `HSI2C_MASTER_ST_LOSE` is used in one place:
  `exynos5_i2c_bus_check` (LX:671-693), called from `exynos5_i2c_message_start` before the transfer is configured (LX:757). There LOSE
  means "bus stuck, SDA low" and triggers recovery (manual READ_DATA + STOP, LX:646-668), not a transfer failure.
- SGS (*read from source*): `HSI2C_MASTER_ST_LOSE` appears only in its `#define` (SGS:175). The polling read looks at RX_FIFO_EMPTY and
  a deadline (SGS:895-916). The IRQ path fails on INT_STATUS bits 8-11 (`HSI2C_INT_CHK_TRANS_STATE`, SGS:733-751) and reads TRANS_STATUS
  only to print it. The state field is compared in two places, neither with 0xC: before a transfer against 0x6 when
  `reset_before_trans` is set (SGS:1074-1082, reset, not failure), and after an interrupt-mode read against INIT 0x1 when
  `scl_clk_stretch` is set (SGS:929-941, SGS:1017-1029, a wait that only logs).
- So neither driver fails a transfer on state LOSE mid-transfer. Both retry on -EAGAIN (LX:903 retries 3, SGS:1278 retries 2); the
  reader never retries and stops on the first failure, which is stricter.
- The log fits a passing state, not a lost bus (*guess* on the cause, *observed* on the words): the second byte was in the RX FIFO, no
  interrupt bit and ERR=0, and the bus went idle by itself (`restore wait idle=1`, after TRANS=00080001). The restore failed only
  because the reader stopped before popping that byte (after FIFO_STATUS=00010100).

## Requirement 2: no new register access, no new write — PASS

- The diff of P is one line inside one expression (P:744). `sfg_poll` still reads INT_STATUS, FIFO_STATUS, TRANS, ERR in the same order
  (P:741-742); TRANS is still needed for NO_ACK and MASTER_BUSY. `sfg_rd_ok`, `sfg_reg_writable`, `sfg_w32_allowed`, `sfg_wait`
  (10 ms and 100000 polls), `sfg_idle_wait`, `sfg_restore` are byte-identical (*observed*: `git show --stat`, 1 line changed in P).
- New behaviour reachable only where the old code returned SFG_ERROR for 0xC: the reader now goes on to steps it already had (RX pop
  after FIFO_STATUS says not empty, idle wait, INT_STATUS clear, next register), each through the same allowlist and bounds.
- *Observed*: `python3 -I tools/test_hsi2c_fg_audit.py` -> 5 tests OK. Host test `violations == 0` on the new case (the fake flags any
  write outside the allowlist) with `restore_tail()` and `as_before()` true.

## Requirement 3: pre-transfer LOSE refusal and TRANS_ABORT — PASS

- `sfg_precondition` is unchanged: state must be idle or init (P:943-944), before any write. T:526 (`h.trans` state = 0xC ->
  "refused master state", zero writes) passes (*observed*). The reader refuses where LX would attempt recovery writes.
- `abort_at` (T:586-588): INT_STATUS TRANS_ABORT -> `why=int-errs`, "refused REPSOC reg=0x06 error", restore run, reader stopped.
  Passes (*observed*). TRANS_ABORT is in `int_errs` (P:422).
- M1 (my mutant, private copy, *observed*): fake raises state 0xC with no TRANS_ABORT bit and delivers no byte -> "refused REPSOC
  reg=0x06 timeout", reader stopped. So a real loss without an interrupt bit still fails closed, 10 ms later than before.
- M2 (*observed*): the old P:744 line put back -> the new case fails with `fail at rd-byte0 why=lose-state`, so the case pins the change.

## Requirement 4: can the result be silently wrong?

In code: no guard (H2). What stands between a corrupted transfer and a printed line is only: both bytes must arrive within 10 ms, no
NACK / error bit / NO_ACK state / ERR_STATUS on any poll, bus idle after the read, and a verified restore (FIFOs empty, not busy,
ERR 0). None of these look at the data.

For a read-only diagnostic run that is enough, provided the PASS is judged by a person against these (none is automated; no tool reads
`src=fg`):
1. `raw 21=` (DEVNAME) equals the expected MAX77759 value (the fake uses the table reset value 0x6200, T:361; confirm on the first log).
   A constant register read last is the best integrity anchor in the set.
2. `v=` within about 100 mV of `fastboot getvar battery-voltage` taken around the same boot (4029 mV on 2026-11-14, docs/AUTOMATION.md:614-616).
3. `soc` 0-100 %, `t` plausible for a phone on a desk, FULLCAPREP (0x35) near the design capacity.
4. Two sets 30 s apart agree (soc and v move by little; DEVNAME identical).
5. No `fail`, `refused`, `BUS NOT IDLE` or `RESTORE FAILED` line.
It is not enough for anything that acts on the values.

## Requirement 5: host runs (*observed*)

- `../pixos-run.sh "bash .../scratchpad/fght/run.sh"` -> `PASS hsi2c_fg_xnu host test`, `exit 0`; the ASan/UBSan build also PASS.
- `../pixos-run.sh "python3 -I tools/test_hsi2c_fg_audit.py"` -> `Ran 5 tests`, `OK`.
- M1 and M2 as above, built from private copies in the scratchpad; the tree was not modified.

## What to expect on the phone

If the c4-batt3 state was transient: seven reads, `raw ... restore=ok`, a `src=fg` line, repeated every 30 s. If 0xC was a real fault:
`fail at rd-byte1` or `rd-idle` with `timeout`, and the reader stops. Power-cycle after any `refused` or `RESTORE FAILED`, as before.
