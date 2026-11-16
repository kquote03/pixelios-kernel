VERDICT: SAFE

# Review: xnu patch 0160 sweet-wlan-rc-busmaster (root port Command before the endpoint's bus master)

Reviewer: Claude Fable 5.2 (independent, worked alone, did not write the patch, no phone access, nothing run on the phone, no code changed). Date 2026-11-16.
Reviewed: `git diff 81d1a4b..67bac14` for `xnu/patches/0160-sweet-wlan-rc-busmaster.patch`, `shim/wlan_wr_host_test.c`, `shim/build.sh`, `tools/test_wlan_wr_mutants.py`, `docs/research/51-wlan-msgbuf-host-dma.md`, `device/google/oriole/docs/PORT.md`. Earlier: `docs/reviews/0159-wlan-msgbuf-review-fable-{1,2}.md`.
Line refs: `glue:N` = `bsd/dev/sweet_wlan.c`, `wr.h:N` = `bsd/dev/sweet_wlan_wr.h` in `~/pixelios-artifacts/localbuild/work/w8/xnu/`; `test` = `shim/wlan_wr_host_test.c`; `mut` = `tools/test_wlan_wr_mutants.py`; `note` = `docs/research/51-wlan-msgbuf-host-dma.md`; Linux = `~/pixelios-artifacts/research-src/linux-v6.18`.
Tags: **[obs]** I ran it and saw it, **[src]** read from source, **[mem]** from memory of the PCI Express specification or the DesignWare core (neither is among the pinned sources), **[guess]**.
The header of the patch chain at `67bac14`, the header the shim build extracts and the patched tree's `sweet_wlan_wr.h` are byte-identical; the chain's `sw3_ptr`..`sw3_wr` text is contained verbatim in the patched `sweet_wlan.c` [obs].

Summary. The patch adds one RC-local word (the root port's Command, DBI 0x04) to what level 8 may read and write. It adds only the memory and master bits to the word it reads, writes the Status half as 0, and puts the recorded word back, after the endpoint's bit is off, on the pass path and on every stop. In all my fault runs the root port word ends at its start value, except where the register itself refuses the write, and that is reported as `restore=failed`. The root port restore is done even when the link is down and the endpoint restore has to be skipped, which is the safer order. Levels 3-7 are unchanged. The diagnosis is the best-supported single explanation of the first run but not the only one; the new print line settles whether it applied. Nothing found is above L. The full mutant suite, which the author had not run for this commit, passes in my run (MUTANT_SUMMARY).

## Findings

| id | sev | finding | required fix |
|----|-----|---------|--------------|
| A1 | L | **If the second run fails the same way, the log again cannot say why.** The patch prints the root port word once, before the change (wr.h:2389-2390). Nothing prints the endpoint's or the root port's Status half after the wait. A request that is sent and rejected sets "Received Master Abort" in the requester's Status (bit 29 of the endpoint's cfg 0x04 word) [mem]; a request that is never sent sets nothing. Both words are already readable at level 8 (cfg 0x04 since 0129, DBI 0x04 by this patch), and the patch correctly leaves the Status bits uncleared, so the evidence would be there. | Recommended, not required: at a run F stop after bus master, read and print cfg 0x04 and DBI 0x04 (reads only). Without it a second `fw r 0` leaves doorbell, firmware state and RC forwarding indistinguishable. |
| A2 | L | **The note's list of next candidates is short.** note s.11 names the doorbell (U5) and the S2MPU (U2). `dw_pcie_setup_rc()` does more to the root port than the Command write: bus numbers primary 0 / secondary 1 / subordinate 0xff (`pcie-designware-host.c:1070-1074`) and the RC's own BAR0/BAR1 (`:1061-1062`) [src]. The shim writes none of them. Completions for the endpoint's memory reads are routed back by bus number [mem]; and with the Memory bit now set on the root port, its own BAR0 starts decoding inbound requests [mem], which matters only if a request address falls inside it (the pool is at 0x8_84xx_xxxx; a BAR that large is unlikely [guess]). Either would show as a failed read, not as a stray write. The shim's row-25 read already has DBI 0x18 (`wlan_seq.h` rows 24/25). | Add both to note s.11 as candidates for a third iteration, with the DBI 0x10/0x18 values from the shim log. No code change. |
| A3 | L | **The glue can refuse the root port restore.** `sw3_wr()` accepts a root port value only if no Command bit other than 0x2/0x4 differs from the live word (glue:271). If another bit has changed since the word was read, the restore of the recorded word is refused. [obs] with the real `sw3_wr()` and a bit flipped in the root port store on its way to the fake (IO bit appears): stop READBACK at W22, restore refused, root port left with the memory, master and IO bits set, `restore=failed`; the endpoint is not a master in that case (the second case is the same flip at the final off store: root port left with only the IO bit). A Command bit that changes by itself is not expected on hardware [guess]. | None. The state is reported and the endpoint is off. If the gate is touched again: let a value that only clears bits 0x2/0x4 relative to the live word pass. |
| A4 | L | **One mutant label is too strong.** `0160: root port word readable below level 8` (mut, EQUIVALENT "for stores") is honest about stores, but a direct `swl_rd(SWW_DBI, 0x04)` at level 7 would tell the mutant from the shipped header (the read table refuses it at run 6, wr.h:376). No such case exists; the test only checks `swl_rd_find()`. Same pattern as review 0159-1 F8. On the phone the glue has no DBI page below level 8 (glue:238, 545), so the read cannot reach hardware either way. | Test-only: add the direct read case and set the entry to `None`. Not needed for the run. |
| A5 | L | **`left after stop` has no line for the root port.** Its state after a stop is visible only in `restore space 7 win 0 off 0x4 := <word> ok/FAILED` and in `restore=failed`. Sufficient with the fail-on list; a reader has to know that space 7 is the root port. | None. |

## Per-item verdicts

### 1. Is the diagnosis sound: PLAUSIBLE, not proven; the next run decides it

- The rule: for a Type 1 function, Bus Master Enable controls forwarding of memory and I/O requests in the upstream direction; with it clear, such requests received at a root port are handled as Unsupported Requests [mem; PCI Express Base, Command register]. Whether this DesignWare core enforces it is [guess]. Supporting [src]: Linux sets the bit on every bridge above an enabled device (`drivers/pci/pci.c:2071-2090`) and the DesignWare host setup writes IO, MEMORY, MASTER, SERR to the root port's Command (`pcie-designware-host.c:1076-1081`); the shim writes only DBI 0x890 (`shim/wlanlink.c:371`; `wlan_seq.h` row 19). The word has never been read on the phone by XNU; the shim reads it in row 24 but I found no recorded value.
- Consistency with the o5-w7-l8 log (PORT.md:104): W18-W22 passed, doorbells were stored, `h2d w 2 fw r 0`, `d2h fw w 0`, ring untouched, guards intact. A firmware whose read of the first ring item is answered with UR never advances its read index and never writes a completion: consistent with every line [src for the log, guess for the firmware's reaction].
- What else gives exactly that log, and what the next run shows:
  - Doorbell not acted on (wrong register for this firmware, U5; or the firmware waits for something the host did not write, such as the interrupt mask, U4, or more host_cap bits, U7): `fw r 0` again with the root port now a master.
  - Firmware in a low-power state: the same; host_cap asked for no deep sleep, so unlikely [guess].
  - S2MPU blocks the read (U2): the same on the host side.
  - Bus numbers or the RC's own BAR (A2): the same.
  - A missing inbound translation: no translation exists on stock either (no `dma-ranges`, review 0159-1 item 12); not a candidate unless requests are forwarded, and then it would show as a completion with wrong bytes or a guard report, not as `fw r 0`.
- Does the print let the run tell them apart: it tells whether the hypothesis applied (root port master off before the change, on after), not which of the others holds if it still fails (A1). See "How to read the second run" below.

### 2. The value written, the Status half, the address: PASS

- Value: `(old & 0xFFFF) | 0x6`, checked against the word `swl_wr()` itself reads (wr.h:1145-1151, the compare at 1147), not the caller's copy. Leaving IO and SERR as found is right: there is no I/O space behind this port, and SERR enable would turn link errors into system errors, which XNU does not handle [mem]. Linux writes all four because it owns the whole root port; here the smaller change is the reversible one.
- Status half written as 0: correct. The Status error bits are write-1-to-clear [mem]; writing 0 leaves them, Linux's read-modify-write clears whichever were set. Leaving them is safer and keeps evidence (A1). [obs] Q5 with Status 0xF910: no store carries a non-zero upper half and the word ends exactly as it started. The glue refuses any value with a non-zero upper half (glue:271).
- DBI write protection: `DBI_RO_WR_EN` gates writes to read-only and hardware-initialised fields; Command is a plain read-write register and needs none [mem]. In Linux the Command write happens inside the `dw_pcie_dbi_ro_wr_en()` window only because the whole function runs inside it (`pcie-designware-host.c:1050-1054`) [src]. If the write does not take on this RC, the read-back fails and the run stops at W22 before the endpoint store ([obs] the author's `rc_stuck` case and my Q4b).
- Address: `SW_DBI 0x14800000` (glue:53) = `soc/google/gs101/soc.json:33` `"dbi"`; the iATU page XNU has used since level 3 is the same block + 0x300000 (glue:50) [src]. Word 0x04 is the only one `sw3_ptr()` returns (glue:237-238).
- Readable in this boot state: the shim reads DBI 0x0, 0x4, 0x8, ... with fault-guarded reads in rows 19, 24, 25 of every `--wlan-hold` boot and stops the chain on the first unreadable word (`wlan_seq.h:245-275`; `shim/wlanlink.c:6`) [src]; XNU reads the iATU page of the same block at every level on hardware (the project's observation). XNU's own read has no fault guard: a fault would be a panic at W22, before any Command store. I rate that very unlikely.

### 3. New residual risk: the same risk as 0159, now expected to be real

- 0159 was accepted on the assumption that the endpoint's requests might be forwarded. If this patch works, they are: for the roughly 0.1-4 s both bits are set, the firmware can read and write any address the RC forwards (below 2^36, with no IOMMU in the path as far as observed). Nothing new is reachable compared with what 0159's review assumed; the probability that a wrong address matters goes from "if forwarding works at all" to "yes".
- The mitigations are the same and are still the right ones: every address the firmware is told is in the pool (R1, unchanged code); the host index words must read 0 (they did: `0 0 0 0 0 0 0 0` in o5-w7-l8, the project's observation); the pool is below 2^36 (0x884d90000 in that run); the guard count is printed at the end and on every stop; both bits are cleared on every exit. None of them can detect a firmware write outside the pool. That residual (a panic, or silent damage to `userdata` that needs a re-flash) is as stated in review 0159-1 item 12 and note s.10, and I accept it on the same terms.
- New and small: the root port also gets the Memory bit, which enables its own BAR decode for inbound requests (A2). That can only swallow a request, not redirect it [mem].

### 4. Order and restore: PASS

- On: root port (wr.h:2391), then endpoint (wr.h:2392); the endpoint tuple now needs the root port tuple done (`pred1`, wr.h:346). Off: endpoint, then root port (wr.h:2422-2423); the root port's off tuple is refused while `SWF_BM` is set (wr.h:1152-1154).
- Restore table: endpoint first, then root port (wr.h:382-383). The root port group has no link gate: `swl_restore()` gates only `SWW_CFG` (link) and `SWW_BAR0` (preconditions) (wr.h:965, 975), and the DBI write is RC-local. So with the link down the endpoint restore is skipped and the root port is still put back. That is the right behaviour: an endpoint that may still have its bit set sits behind a port that no longer forwards.
- [obs] Q7: link down at each of the 63 run F stores, both read models: the endpoint bit is left in 34 of 126 cases (all reported), the root port is back at its start word in all 126. Q8: the endpoint's bit sticks at its restore: endpoint still a master, root port back, `restore=failed`. Q4a/b/c: a stop exactly between the two on-stores (endpoint store refused, dropped, link drop at the root port store): root port back, endpoint never a master, no doorbell.
- [obs] the author's sweep (each of 63 stores failed or dropped; `bm_off()` now also requires the root port word to equal its start value) passes in the host test. Mine through the real `sw3_wr()`: 63 x {failed, dropped}: 0 bad; with a flipped bit: 2 cases where the glue refuses the restore (A3), reported. Each of the 1852 run F reads failed once (Z10): bus master off and root port back in all.
- [obs] Q3 / Q9: the root port's master bit sticks at the off store or at the restore: endpoint off, `restore space 7 ... FAILED`, `restore=failed`.

### 5. Writer rules, read table, levels <= 7: PASS

- On: value must be `old | 0x6` (wr.h:1147-1151); records `rc_cmd_old` and `rc_have` only after that check; once; needs W21. Off: needs the on tuple done, a recorded word, exactly that word, and `SWF_BM` clear (wr.h:345, 1152-1154). The generic rules still apply: preconditions, all-ones old value stops as dead ([obs] Q2), read-back under mask 0xFFFF.
- [obs] Q1: the word changes between W22's read and the writer's read: stop OLD, no store to either Command. Q5: start words 0x6, 0x4, 0x2, 0x147, 0x107, Status 0xF910: pass, word back exactly. Q6: second stage-3 call: no Command store.
- Read table: one entry, run 6, DBI 0x04 only (wr.h:376); `swl_wr_ok()` (runs <= 5) does not see the new tuples; the value-family flag `SWT_RC` joins the dynamic-value branch (wr.h:832).
- Levels <= 7: [obs] my probe (host test and chain at `81d1a4b` against `67bac14`, real `sw3_wr()` in front of the fake, levels 3-7, both boards): outputs identical, 1330 lines, 0 refusals; the old side equals round 2's new side byte for byte.

### 6. Glue: PASS

- `hw.dbi` is mapped only at level >= 8 (glue:545) and is NULL otherwise, so `sw3_ptr()` returns NULL for the space and both `sw3_rd()` and `sw3_wr()` refuse (glue:237-238). The gate (glue:271) tests `h->dbi != NULL` before it reads the live word. Conditions: level 8 after the release, offset 0x04, upper half 0, only bits 0x2/0x4 differing from live, master only with a pool.
- [obs] the glue sweep (10 levels x release x pool x mapped x 4 live words x 9 values x 3 offsets), the 7 new glue mutants and the three re-pinned old patterns (text only: one more space in the first test, one more flag in the family) pass in the full mutant run. G1: a full level 8 through the real `sw3_wr()`: 0 refusals. G2: glue without a pool refuses the root port's master store; the run stops at W22 with no endpoint store.

### 7. Mutants and host-test cases: PASS (A4)

- 13 header mutants: value rule, three terms of the off rule, the predecessor, restore armed / entry / value / order, the two step calls, the read-table run, the family flag. 12 are `None` and killed in my run; one is labelled EQUIVALENT (A4).
- New cases cover: other bits kept, already a master, all-ones, bits not taken, a stop with both on (restore order checked in the log), six direct-call refusals, level 7 has no tuple. The fake firmware now needs both master bits to touch host memory (test, `fw_msgbuf`).

### 8. Worst case and the run command: see below

- Worst case: unchanged from 0159 (a device write outside the pool; panic or `userdata` damage), now with forwarding expected to work. Next: a panic at the first DBI read (item 2). The root port left a master after a failed restore is harmless by itself once the endpoint's bit is off, and a power cycle clears it.

## Commands and results

| Command | Result [obs] |
|---|---|
| `cd shim && PIXELIOS_HOST_TESTS=always ./build.sh --board oriole --avb --pstore-fast --usb-gadget --out out/rev0160` (heavy lock) | rc 0; `PASS wlan_wr_host_test (compile)`, `RESULT: PASS wlan_wr_host_test`, `PASS wlan_wr_host_test`; 302 PASS lines |
| `gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror -Iout/rev0160 -o /tmp/claude-1000/t0160 wlan_wr_host_test.c && /tmp/claude-1000/t0160` | `RESULT: PASS wlan_wr_host_test`, rc 0, no sanitizer report |
| `python3 -B tools/test_wlan_wr_mutants.py` (full, heavy lock) | MUTANT_ROW |
| `tools/patch-numbers check` | `RESULT: PASS this tree's patch numbers do not overlap other branches or reservations` |
| `strings -a ~/pixelios-artifacts/xnu-local-w8-67bac14/kernel.development.sweet \| grep 'root port Command'` | `SWEET WLAN: W22 root port Command/Status %#x (memory %s, bus master %s)`, `SWEET WLAN: W25 root port Command back at %#x` (sha256 starts `b27e8e7e95f1cfbd`) |
| Probe R8 (levels 3-7 before/after, real `sw3_wr()`) | identical, 1330 lines, 0 refusals |
| Probe G (level 8 through the real `sw3_wr()`) | G1 rc 0, 0 refusals, 39 chip-RAM stores; G2 stop at W22; G3 63 x {failed, dropped}: 0 bad, bit flipped: 2 refused restores (A3) |

Scenarios (scratch copy of the host test with my fake knobs, ASan/UBSan build):

| id | scenario | result [obs] |
|---|---|---|
| Q1 | root port word changes between the read and the store | stop OLD at W22; 0 root port stores, 0 endpoint stores |
| Q2 | root port reads 0xFFFFFFFF | stop (dead read) at W22; nothing stored to either Command |
| Q3 | root port master bit sticks on at the off store | stop READBACK at W25, endpoint off, `restore space 7 ... := 0 FAILED`, `restore=failed` |
| Q4a/b/c | stop between the two on-stores (endpoint store refused / dropped / link drop at the root port store) | root port back at its start word, endpoint never a master, no doorbell |
| Q5 | root port already 0x6, 0x4, 0x2, 0x147, 0x107, Status 0xF910 | pass; word ends exactly as it started; no store with a Status bit |
| Q6a/b | second stage-3 call (same / fresh context) | stops at W1; no Command store on either device |
| Q7 | link down at each of 63 run F stores, two read models | endpoint bit left in 34 of 126 (reported); root port back in 126 of 126 |
| Q8 | endpoint master bit sticks at the restore | endpoint still a master, root port back, `restore=failed` |
| Q9 | timeout, then the root port bit sticks at the restore | endpoint off, `restore=failed` |
| Z1-Z15, N1-N5 of 0159 | again on the new header | same results, with the root port back in every case that checks it. Two changes that are not findings: Z14 (endpoint bus-master tuple called directly on a level-7 state) is now refused as ORDER instead of PHASE, with no store; the link-down sweep completes normally when the drop comes at the last store, which is now the RC-local root port store |

## The run command for the second level-8 run

```
SWEET_USBLOG_PORT=64738 tools/xrun --kernel ~/pixelios-artifacts/xnu-local-w8-67bac14/kernel.development.sweet --board oriole --no-camera --gui --rails --cpus 8 \
  --wlan-hold --wlan-level 8 --wlanfw ~/pixelios-artifacts/wlan-fw/wlanfw-oriole.bin --intent wlan \
  --expect 'SWEET WLAN: (stage 3 level 8 (done|stopped)|fw version)' --fail-on 'restore=failed|DMA suspect|bus_master=ON|bus_master=unknown|restore of group 16 skipped' --fail-on 'panic\(cpu' --fail-on 'Kernel trap at' --label o6-w7-l8b --restart 600,bootloader --lock-wait 3600
```

The kernel must be the one with sha256 `b27e8e7e...`. The label is my suggestion. The command is the reviewed 0159 one with the new kernel; no fail-on needs adding (a failed root port restore prints `restore=failed`).

How to read it:

| Log | Meaning | Next |
|---|---|---|
| `W22 root port Command/Status ... bus master off`, then `W24 device-to-host DMA works`, `fw version ...`, `W25 root port Command back at ...`, `stage 3 level 8 done` | U8 confirmed; U1, U2, U5 settled with it | PASS. Record the root port word and the pool address |
| `... bus master off`, then `no completion ... fw r 0 ... untouched` | The root port bit was clear and setting it was not enough (or the core does not enforce it). Candidates: doorbell / firmware state (U4, U5, U7), bus numbers or RC BAR (A2), S2MPU (U2) | Third and last iteration; do A1 first so that run can tell them apart |
| `... bus master on` before the change, then `fw r 0` | U8 is wrong; the run was the first run again | As the row above, without the RC candidates that depend on the master bit |
| `fw r 2` (or 1) but no completion, ring `untouched` | Reads from host memory work, writes to it do not arrive | S2MPU or write path; stop the ladder as the note says |
| `fw r` moved and `has bytes` or a `completion does not match` stop | Device writes arrive; a protocol detail is wrong | Read the printed item; new review for any change |
| stop at W22 with `dead read` | The DBI word read all-ones | Nothing was written; the DBI page is not what the shim saw |
| stop at W22 with `read-back` and `restore space 7 ...` | The root port did not take the bits | Endpoint never a master; U8 cannot be tested this way |
| `guard bytes changed N` with N > 0, or `DMA suspect` | A device write landed in the pool outside a ring or buffer | H10 counts it; re-flash `userdata` |
| any `panic` / `Kernel trap` | If before `W22 bus master ON`: not a DMA effect. If after: H10 counts it | Re-flash `userdata` before trusting the image |

## What I could not verify

- That this DesignWare root port drops upstream memory requests while its Bus Master Enable is clear, and that its Command register is writable through DBI without further steps: specification and core behaviour from memory; the read-back and the next run decide.
- The value of the root port's Command on the phone: never printed by XNU; I found no recorded shim value.
- That a read of DBI 0x04 from XNU cannot fault: inferred from the shim's guarded reads of the same word in the same boot state and XNU's reads of the same block.
- Everything about the real firmware and the S2MPU that review 0159 could not verify remains unverified.
- `tools/xrun` was not run by me.
