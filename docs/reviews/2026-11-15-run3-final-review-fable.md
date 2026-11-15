VERDICT: FINDINGS

# Final review of unattended run 3 (2026-11-15), oriole

Reviewer: Claude Fable 5.2 (worked alone, read-only apart from this file; no phone access, no build, no commit, no push).
Scope: pk-dev `wip/oriole-next` 506a64b..38cef14 (pushed), the later local commit 6853ea7 (docs only, read), worktrees pk-bt and pk-amp,
run directories `logs/runs/20261115T*`, `~/.hermes/cache/scratch/{o3-*,c4-batt2*,usbnet-watch.log,review/}`, the host kernel journal for 07:10-08:10.
Tags: *observed* (a file, command or log I read myself), *read from source*, *guess*.

No safety limit was crossed and nothing forbidden is in git. The findings are about process (hardware-writing runs made without a log
channel) and about claims that the saved evidence does not support. No H finding.

## Findings

| id | sev | finding | fix |
|---|---|---|---|
| R1 | M | Three runs of a hardware-writing kernel (`sweetbatt=1`, 0148, HSI2C12 controller stores) were made with no log channel. `c4-batt2` was the first boot of the new local kernel ab30eb13 and already carried `sweetbatt=1` (plan Block P: first run of a new kernel is a baseline without new args). After it returned 0 bytes, `c4-batt2b` repeated it; after the baseline `o3-base-14c9780` also returned 0 bytes, `c4-batt2c` ran the writer a third time. 0148 ended run 2 with `restore=FAILED` / `BUS NOT IDLE` (o2-c4-batt1), so the H7 condition could have occurred three times unseen. Mitigating: the delta was reviewed SAFE before the first run, the writer touches only the HSI2C12 controller, its state is lost on reset, and the phone came back to fastboot each time (*observed*, journal: fastboot device at 07:25:45, 07:38:16, 08:04:17). | Rule for the next run, in PORT.md and docs/AUTOMATION.md: no run with a writer boot-arg until a baseline on the same host session has delivered log bytes; a 0-byte run of a writer is followed by a baseline, never by a repeat. Treat the C4 state as unknown (not as "no result"): the next C4 run starts from a cold fastboot boot. |
| R2 | M | "28 run E mutants killed" (PORT row o3-w5, tagged observed) and "25 mutants (all killed)" (progress file) are not supported by the saved evidence. `o3-w5-mutants.out` (07:36) FAILED (store-path audit), `o3-w5-mutants2.out` (08:07) FAILED with `run E W16: pointer may reach the last word: SURVIVED`. The PORT row was committed at 08:05, before that result existed. The fix bfafa64 (08:16) has no saved rerun; `shim/out/wlan_wr_host_test` is from 07:52, so the new CHECK was not even run on the unmutated code through `shim/build.sh` (*observed*, mtimes). The pushed head therefore contains a host-test line whose pass state is unknown; if it fails, `shim/build.sh` fails. Review 2 of 0150 covers 4506d56..ef93d09 and not bfafa64 (test file only, no kernel text). | Before any other work: build the shim host tests and run `tools/test_wlan_wr_mutants.py` at HEAD, save the output, correct the PORT row to the result. Until then read the row as "27 of 28 killed at ef93d09 (observed); 28th: test added in bfafa64, unrun". |
| R3 | L | The "host firewall drops UDP 5140" diagnosis (tagged guess, correctly) rests on one run, not four, and the row hides that. *Observed* in the journal: the phone enumerated as `Sweet iOS (USB Ethernet)` in all four runs (07:16:37, 07:29:37, 07:41:38, 07:55:39), each about 1 min 55 s after boot. `usbnet-watch.log` has an address activation only at 07:55:39 (twice: two watcher instances). So in c4-batt2, c4-batt2b and the baseline the interface had no 192.168.7.1 and 0 bytes is fully explained by that; "Baseline ...: same, 0 bytes" reads as if the baseline were a valid control, which it is not. Only c4-batt2c had the address and still got 0 bytes. Parts I confirmed: `/run/current-system` relinked 04:40:05, and `allowedUDPPorts` has no 5140 (*observed*). Not explained by the guess: run 2 received logs last night on the same system generation 45 (*guess*: a runtime rule lost at the 04:40 activation). | Reword the row: enumeration in all four runs (observed), address configured only in c4-batt2c, baseline not a control. Next run: start one watcher, confirm the address is on the interface, then capture on the interface (does a datagram arrive at all) before changing the firewall; the firewall change needs the user (root). |
| R4 | L | The progress file disagrees with the evidence and is incomplete: "c4-batt2b watcher running, still no enumeration" (journal: enumerated 07:29:37; PORT says the watcher was dead); no lines for `o3-base-14c9780`, `c4-batt2c`, the 0150 review verdicts, or the stop of all phone work. | Correct the c4-batt2b line and add the missing lines. |
| R5 | L | Open findings are not all in a place the next run reads first. 0148 review 2 G1-G4: PORT row o3-c4-f5 names them (ids only). 0150 review 1 F3 (F8 was closed by evidence in review 2): PORT row o3-w5. 0150 review 2 verdict and G1-G6: not in PORT.md (the row says "round 2 see ..."), only in the review file and in the unpushed docs/AUTOMATION.md commit 6853ea7. WLAN F2, F4, F6-F9: only in the run-2 report (line 38); not in PORT.md, the progress file or AUTOMATION.md. No run-3 report exists yet. | List all of them in the run-3 report and in one PORT.md row: 0148-2 G1-G4 (L); 0150-1 F3 = 0150-2 G1 (required before any store level after the CA7 release), G2-G6 (L); WLAN F2, F4, F6-F9. Put the round-2 verdict (SAFE) in the o3-w5 row (plan Block V). |
| R6 | L | HEAD is 6853ea7, one commit after the reviewed and pushed range (docs/AUTOMATION.md, the `sweetwlan=7` section; docs only, *observed* `git diff --stat 38cef14..HEAD`). The fork's AUTOMATION.md therefore has no `sweetwlan=7` section. The section's command uses the local kernel `xnu-local-ef93d09`; plan Block K wants a CI kernel for recorded results. | Push 6853ea7 with the next fast-forward push; run level 7 on a CI kernel of the pushed head (after R2), or record why a local kernel is used. |
| R7 | L | Pre-run battery voltage (H2) is kept for the session start (4232 mV, progress file) and for c4-batt2c (4198 mV, `c4-batt2c.out`) only; the other three runs have none in their logs. No post-run temperature (H3) could be read (no log, no recovery read). Nothing suggests a limit was near. | Keep the `battery-voltage` line in every run's output; note in the report that H3 was not checkable in run 3. |
| R8 | L | c4-batt2b and c4-batt2c were staged from a dirty tree (`run.json`: `d8f3cc3+dirty`, `1614fda+dirty`; plan: never run an image built from an unknown tree). The kernel was the prebuilt ab30eb13 in all three (*observed*, same sha256), and the range changes no shim source that reaches the phone (`shim/build.sh` host-test section and two `*_host_test.c` only), so the booted code was the reviewed one. | Commit or park work before staging a run; no further action for these runs. |

## Answers

1. **SAFE review before every hardware-writing run: PASS.** Only one writer ran: 0148 with the F5 diagnostics, kernel `xnu-local-6b78437`
   sha256 ab30eb13 (*observed*: `o3-local-c4.out`, built 07:07:21 from the stack one minute after commit 6b78437 07:06:10). The reviewed
   diff `review/0148-f5.diff` is byte-identical to `git diff 506a64b..6b78437` (*observed*, `cmp`). Review file first line `VERDICT: SAFE`,
   committed 6b82d2b at 07:13:14; `c4-batt2` started 07:13:35 with `git_head 6b82d2b`, image built 07:14:38, booted 07:14:41. c4-batt2b
   (07:26) and c4-batt2c (07:52) used the same kernel. `o3-base-14c9780` used CI kernel f118508a without `sweetbatt`. 0150 (kernel
   3a5c456b) appears in no `run.json`: never ran. 0147 and 0149 in the stack were covered in run 2. See R1 for how the runs were made.
2. **Hard stops and limits: PASS** (with R1, R7). All four boots were `fastboot boot` of a RAM image (*observed*, `xrun.log`); no flash,
   erase or oem command in any run log. The added lines of the range contain no access to I2C 0x69/0x25/0x57/0x66, no PMIC, GSA, Trusty or
   amplifier code; 0150 writes no EP Command and no BAR2 word after W14 (*read from source* and both reviews). B2/A1 were not authored;
   pk-bt and pk-amp are at 6c165fb, clean. H4: three no-data C4 runs, then phone work stopped.
3. **PORT.md tags: FINDING** (R2, R3). Checked and correct: kernel hashes ab30eb13 and 3a5c456b, review file names and verdicts, F1/F2
   fixed in ef93d09, c4-batt2c enumeration 07:55:39-08:03:46, the relink time 04:40, the missing port 5140, phone left in fastboot
   (journal 08:04:17), worktrees without commits.
4. **Nothing forbidden in git or pushed: PASS.** The range touches 12 files (docs, tests, two patches, two scripts); no firmware, binary,
   dump or credential. The phone's USB serial (seen in the host journal) has 0 hits in the tracked tree, the run logs of this session and
   the plan files. `logs/` is ignored. The MAC-like string in the tools is the gadget's fixed locally administered address, not a hardware
   address, and is not in the range. `git ls-remote fork`: `wip/oriole-next` = 38cef14; reflog: one push, 14c9780 -> 38cef14 at 08:18:43,
   fast-forward; no other fork ref changed this session; nothing went to `origin`.
5. **Touch defaults: PASS.** `git diff 506a64b..38cef14 -- device/google/oriole/board.json tools/test_mkadt_touch.py tools/mkadt.py` is
   empty; also empty from 14c9780.
6. **Patch numbers and sweetwlan=7: PASS** (with R6). `tools/patch-numbers check`: `RESULT: PASS this tree's patch numbers do not overlap
   other branches or reservations`; 0147-0150 are contiguous. `tools/bootargs-known.txt` lists `sweetwlan patch:0124` (the file holds
   names, not levels; `sweetbatt patch:0148` present). `tools/build-xnu-image.sh` accepts `--wlan-level 7`. docs/AUTOMATION.md has the
   `sweetwlan=7` section at HEAD 6853ea7, not at the pushed 38cef14.
7. **Open findings visible to the next run: FINDING** (R5).

## Not verified

- Whether the host tests and the mutant suite pass at HEAD (no build allowed; R2).
- The firewall state itself (needs root) and whether any datagram reached the interface in c4-batt2c.
- What 0148 did on the phone in the three C4 runs (no log).
- The user brief of 2026-11-15 named in the progress file (not available to me); I took the C4 rerun as allowed by it, since run 2 ended
  area C for that run only.
