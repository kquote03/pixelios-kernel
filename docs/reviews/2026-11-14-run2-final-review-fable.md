VERDICT: FINDINGS

# Final review of unattended run 2 (oriole), 2026-11-14 22:41 .. 2026-11-15 01:40 +04

Reviewer: Claude Fable 5.2, worked alone, read-only apart from this file. No phone access, no build, no commit, no push.
Scope: pk-dev `wip/oriole-next` 8817998..ebc8eaa (31 commits, includes the merge of `wip/oriole-c4` 09235cb..906d5be), `$W/pixos-lb-fhs.nix`,
run directories `logs/runs/*o2-*` (11 runs), `logs/phone/20261114T18..21*`, `~/.hermes/cache/scratch/o2-*.out`, the plan, the progress file and the draft report.
pk-peri: last commit c840a56 at 16:26, before START; unchanged.
Claims below are tagged *observed* (a log line, git output or tool run by me), *read from source* or *guess*.

No finding is a hardware-safety breach: no write outside a reviewed allowlist ran, nothing was flashed, nothing forbidden is in git.
The findings are one M (two tool safety refusals and two unexpected recovery boots that the report does not mention) and eight L (records and wording).

## Findings

| id | sev | finding | fix |
|---|---|---|---|
| R1 | M (process, record) | `tools/phone goto fastboot` ended twice with `Safety: other-device detected; refusing commands` / `RESULT: FAIL state=other-device target=fastboot steps=1`: after o2-w4-mmu1 (`logs/runs/20261114T192956Z-o2-w4-mmu1/xrun.log`, `logs/phone/20261114T193440Z/phone.log`) and after o2-base-c18dea7 (`...T195801Z.../xrun.log`, `logs/phone/20261114T200119Z/phone.log`) [observed]. In both cases the software reset with `sweetrestart=600,bootloader` brought the phone up in **recovery**, not fastboot; the next invocation found `state=recovery product=oriole`, `getprop ro.product.device` = `oriole`, and ran `adb reboot bootloader` (`logs/phone/20261114T194018Z`, `...T200545Z`) [observed]. So the "other device" was the oriole itself while recovery was enumerating (guess: phone-state saw 18d1 with adb not yet in `recovery` state), and H7's wrong-device clause was not truly met. But the plan's H7 text is "a tool reports a device other than oriole -> stop all phone work", the tool did report it, phone work continued, and neither PORT.md, the progress file nor the report records the two refusals or the two recovery boots. The report also says "no recovery hop", which is wrong, and gives that as the reason temperature was not measured. | Add to the report and to the PORT.md rows of o2-w4-mmu1 and o2-base-c18dea7: the two refusals, that the phone was in recovery, and how it was identified as oriole. Remove "no recovery hop". Open a follow-up: why a bootloader reset lands in recovery in 2 of 11 runs, and make `tools/phone-state` not report OTHER-DEVICE for a target phone that is still enumerating (retry before the verdict). |
| R2 | L (process) | The kernel that ran the writer levels is not exactly the reviewed tree. The 0147 review covers `6b1da78..96164e1`; runs o2-w1-l4, o2-w2-l5, o2-w3-l6 used local kernel xnu-local-fa4999d and o2-w3-l6ci used CI c18dea7, both = 0147 + **0149**, and 0149 edits `sweet_wlan_init()` in the same file as the writer, without a review (it was classed read-only). Also, a kernel containing 0147 booted twice before the review commit c18dea7 (23:36:24): o2-base-localfa4 (23:24, no `sweetwlan`) and o2-w4-mmu1 (23:29, `sweetwlan=2`) [observed]. No changed writer code was reachable in those two runs: 0147's hunks are in `sweet_wlan_stage3`, `swl_restore`, `swl_stop`, `swl_wr`, `swl_w13` and constants, all behind level >= 3, and `hw.level` is capped at 2 for stage 1 [read from source]. Section 7 below is the missing look at 0149: no store. | None for the hardware. Rule for the next run: a patch that touches a file holding a reviewed writer goes to the same reviewer as a delta, read-only or not; and a kernel carrying an unreviewed writer change is not booted with that writer's boot-arg at any level. |
| R3 | L | The last GUI run, o2-c4-batt1, does not show the full baseline health criterion: `SBCoverSheet` 1, `panic(` 0, `hfs_mountroot failed` 0, but `SWEET TOUCH: scanning` 0, because the run stopped at +142 s on its expect lines, before the touch driver's 45 s start delay ended (`SWEET TOUCH: gs101 FTM5 driver, mode 2, starting in 45 s` is its last touch line) [observed]. The report's row uses o2-base-14c9780 (same kernel, six minutes earlier), which meets it in full. Touch after the failed HSI2C12 transfer was therefore not observed. | State this in the report row. On the C4 rerun, add `--expect 'SWEET TOUCH: scanning'` so the run does not stop early. |
| R4 | L | Battery numbers without a log line: "4094 -> 4091 mV" (PORT.md Phase 0 row), "battery 4122 mV (lowest seen 4091 mV)" (report). Under `logs/` and the scratch outputs I find only 4107, 4116, 4106, 4095, 4093, 4104 mV (`RESULT: PASS battery-voltage=...` in six `o2-*.out` files); no `battery_mv` in any `run.json` (Block P's `--record` left nothing) [observed]. The final 4122 mV matters: it is the only sign that the bootloader could still talk to the fuel gauge on I2C12 after `restore=FAILED`. | Quote the log line for each value or mark the value as not logged. When the user is back: `tools/phone-battery --record` once, and keep the output. |
| R5 | L | `docs/AUTOMATION.md:436-455` still says the `sweetbatt=1` reader is "BUILT, UNRUN, awaiting an independent review" and "not run yet", and its status table and the PORT.md row "built, unrun (awaiting review)" are stale: it was reviewed SAFE (906d5be) and run once, FAIL with `restore=FAILED` (o2-c4-batt1). The rule from review 0148 F7 (power-cycle before any further HSI2C12 experiment after `restore=FAILED`) is in the report's retry advice but not in AUTOMATION.md. | Update the heading, the "run form" sentence and the table row; add the F7 rule and the o2-c4-batt1 result. |
| R6 | L | Run counts disagree: PORT.md row o2-w3-l6ci says "runs used 4/45", the report says W "6 / 45"; the W-labelled runs are 5 (w4-mmu1, w1-l4, w2-l5, w3-l6, w3-l6ci). The report's review row says "4 writing runs" and then lists W1-W3 on two kernels plus C4, which is 5 writing runs (4 WLAN + 1 fuel gauge) [observed]. Total phone runs: 11. | Use one count: W 5 runs (plus 1 baseline if counted there), C 1, baselines 5; "5 writing runs". |
| R7 | L | Tagging and quoting. Report rows "Every hardware-writing run had a prior SAFE review", "Repo gate" and "Pushes" carry no observed / read from source / guess tag; "Phone state at end" and "Kernels" lines have none either. The gate row's "500 tests: 6 failures = the known baseline" (delegate run on pk-c4 128320d) has no log under `logs/` or the scratch directory; the only gate output there is `o2-gate-final.out`: `GATE FAIL ebc8eaa unit (timeout)`. `$S/o2-gate-baseline.txt`, which the plan asks for, does not exist. PORT.md W4 row quotes `'atu in r0..r3 cr1 0 ...'`, which is a merge of four lines, not a log line [observed]. | Tag the rows; mark the 500-test result as "reported by the delegate, log not kept" or rerun the unit stage with a longer timeout and keep the log; save the baseline list; quote one real `atu in r0 ...` line. |
| R8 | L | Patch 0149 (section 7): no store, bounded, but (a) the S2MPU address 0x14480000 and length are literals in the C file, not checked against the ADT or soc.json like every other region of `sweet_wlan.c` (`sw_prop_is`); the only board tie is the `/sweet-wlan` compatible string; (b) a read of an unclocked or EL2-protected S2MPU block would be an SError, i.e. a panic (the same class as WLAN F4); it did not happen in two runs; (c) the line `sysmmu none (DT use-sysmmu=false)` prints a fact read from source as if the kernel had measured it; (d) the probe runs before stage 3, so with `sweetwlan>=4` it sits in a boot that writes (o2-w3-l6ci did that). | Before 0149 is kept past `wip/*`: put the S2MPU range in soc.json / the ADT node and check it; reword (c) to "not probed"; record (b) next to F4. No fix needed for a rerun. |
| R9 | L | State after H7: the phone was left in fastboot after a software reset with the fuel-gauge transfer unfinished (one RX FIFO byte, no STOP; review 0148 F7 asks for a power cycle). I2C12 also carries the charger 0x69, TCPC 0x25 and PMIC 0x66. That the reset cleared the controller is a guess in PORT.md, and no later XNU boot checked the handoff words (correctly: H7). The report's retry advice says "after a power cycle" but the DEFERRED-FOR-USER list does not say the phone needs one now. | Add to DEFERRED-FOR-USER as the first item: power-cycle the phone (not a fastboot reboot) before any further run that touches I2C12, then check `fastboot getvar battery-voltage`. |

## 1. Prior SAFE review for every hardware-writing run: PASS (see R2)

| Run (UTC dir time, +04 start) | Kernel | Boot-arg | Review commit (+04) | Before the run |
|---|---|---|---|---|
| o2-w1-l4 (19:39:48Z, 23:39:48) | xnu-local-fa4999d a2f311c0 | `sweetwlan=4` | c18dea7 23:36:24, `docs/reviews/0147-wlan-fw-ident-review-fable-1.md`, line 1 `VERDICT: SAFE` | yes, 3 min 24 s; run.json `git_head` c18dea7 |
| o2-w2-l5 (19:45:15Z) | same | `sweetwlan=5` | same | yes |
| o2-w3-l6 (19:51:13Z) | same | `sweetwlan=6` | same | yes |
| o2-w3-l6ci (20:05:16Z) | CI c18dea7 13ba25f7 (run 43633015714, success, head c18dea7) | `sweetwlan=6 sweetpcie1mmu=1` | same | yes |
| o2-c4-batt1 (20:58:52Z, 00:58:52) | CI 14c9780 f118508a (run 43639157923, success, head 14c9780) | `sweetbatt=1` | 906d5be 00:44:59, `docs/reviews/0148-hsi2c-fg-review-fable-1.md`, line 1 `VERDICT: SAFE` | yes, 13 min 53 s |

All observed (`git log --date=iso`, `run.json`, `gh run view`). The reviewed bytes are the bytes that ran: blob ids of `0148-sweet-hsi2c-fg.patch`, `tools/mkadt.py`,
oriole `board.json` and `soc.json` are equal at 128320d (reviewed), 14c9780 (CI build) and ebc8eaa; `0147-...patch` is the same blob at 96164e1 and ebc8eaa [observed].
0149 is read-only [read from source, section 7]; its run o2-w4-mmu1 used `sweetwlan=2 sweetpcie1mmu=1` and logged `stage 1 done level 2 (read-only)` and no stage 3 line [observed].
None without a prior review. The gap between "reviewed tree" and "run tree" is R2.

## 2. Hard stops and limits: PASS for the limits, FINDING R1 for H7's wrong-device clause

- Charger / TCPC / PCA9468 / PMIC: no code in the range addresses 0x69, 0x25, 0x57 or 0x66. 0148 pins device 0x36 and seven register pointers; the 0148 review found one writer and no data-byte write [read from source, review]. The run log shows `reader armed (HSI2C12 0x10d50000, device 0x36)` and one refused read [observed].
- Flashes: no `fastboot flash|erase|oem|set_active` in any `logs/phone/20261114T18..21*` or `o2-*` `xrun.log`; the newest `logs/flash-*` file is from 2026-11-12 [observed]. Runs are `fastboot boot` only (`Sending 'boot.img' ... Booting`).
- Secure world and amplifier: no A step ran; no GSA/Trusty/amp code exists in the range (doc 50 is research text) [observed].
- H7 after o2-c4-batt1: honoured. `!!! RESTORE FAILED` at 1794690100.755, `reader stopped, no further HSI2C12 access this boot`; o2-c4-batt1 is the last run directory. The six later `logs/phone/20261114T2105*` entries are the gate's `tools/test_phone_sm.py` with fake tools (`/tmp/phone_sm_test_*`, product bluejay), not the phone [observed].
- H2: every recorded voltage is above 4090 mV. H3: no temperature was read (see R1: recovery was reached twice, so it could have been). H4/H5: 0 `panic(` and 0 `hfs_mountroot failed` in all 11 `usbnet.log` files [observed].
- Wi-Fi limits: level 6 stores the reset vector; no ARM release exists in the code (W5 not written) [read from source]; `bus master off` in both probe runs [observed].

## 3. Tags and quoted lines: FINDING R4, R6, R7 (quotes spot-checked: 24, all present except those in R4/R7)

Checked with `grep -c -F` on the run's `usbnet.log` [observed]:

| Claim | Run | Count |
|---|---|---|
| `SWEET TOUCH: scanning, 1080x2400 raw -> 1080x2400`, `SBCoverSheet`, 0 `panic(`, 0 `controller ready` | o2-base-b6697f7 | 1, 1, 0, 0 |
| `SBCoverSheet`, `SWEET TOUCH: scanning`, 0 `panic(` | o2-base-local1, o2-base-localfa4, o2-base-c18dea7 | 1, 1, 0 each |
| `SWEET PCIE1 MMU: ep cmd/status 0x100002 bus master off; sysmmu none`, `s2mpu_hsi2 CTRL0=0`, `FAULT_STATUS=0`, `done (read-only)`; four `atu in rN cr1 0 cr2 0 base 0:0 limit 0xfff target 0:0` | o2-w4-mmu1 | 1, 1, 1, 1; 4 |
| `W5 RAM 0x2a0000 bytes, ram_end 0x4a0000`, `W6 wrapper 0x18120000 after reset: ioctl 0x21 reset_ctl 0`, `W8 cfg 0x84 := 0 (window restored)`, `stage 3 level 4 done crc=0x92567396 words=441023` | o2-w1-l4 | 1 each |
| `W11 NVRAM 24552 bytes -> varsz 21385 varsize 21388 token 0xeb1c14e3`, `W12 token 0xeb1c14e3 at bp 0x49fffc read back`, `W13 rTLV bp 0x49abe8..0x49ac6c`, `stage 3 level 5 done vars=21388 token=0xeb1c14e3 crc=0x23bf8f85` | o2-w2-l5 | 1 each |
| `W14 backplane 0 reads 0xba3ef000 before the store`, `W14 reset vector bp 0 old 0xba3ef000 := 0xbbbef214 read back 0xbbbef214`, `stage 3 level 6 done w14 old=0xba3ef000 new=0xbbbef214 ...` | o2-w3-l6 | 1 each |
| `s2mpu_hsi2 CTRL0=0`, the W14 line, `stage 3 level 6 done` (area W furthest PASS) | o2-w3-l6ci | 1 each |
| `SBCoverSheet`, the full scanning line, 0 `panic(`, `SWEET BATTERY: static fallback, 100% 4200mV`, 0 `SWEET BATTERY: reader` | o2-base-14c9780 | 1, 1, 0, 1, 0 |
| `reader armed (HSI2C12 0x10d50000, device 0x36)`, the ten handoff words, `after ERR=00000000 FIFO_STATUS=00010100 TRANS=00080001`, `!!! BUS NOT IDLE`, `refused REPSOC reg=0x06 error restore=FAILED`, `reader stopped, no further HSI2C12 access this boot` | o2-c4-batt1 | present, values equal the PORT.md row |

Other evidence: kernel sha256 prefixes 2ec258c0, 882a1e0c, 88444aa3, 13ba25f7, f118508a match the files in `~/pixelios-artifacts`; "139-141 patches" matches `o2-local{1,2,3}.out`;
`tools/test_wlan_wr_mutants.py` `Ran 8 tests ... OK` is in `o2-w0-mutants2.out`; both CI runs exist with conclusion success [observed].
Area furthest PASS: W = o2-w3-l6ci (checked), I = o2-base-local1 (checked), C = C3 from run 1 (not rechecked), G and A/B = desk work only (docs/research/49, 50 exist).
PORT.md rows added this run all carry "observed:", "read from source" or "guess" where they claim something.

## 4. Nothing forbidden in git or pushed: PASS

- `git diff --numstat 8817998..ebc8eaa`: 21 text files, no binary. No MAC/BD address pattern, no SSID/PSK/password, no private key, no serial, no home path in the added lines. The eight 40+ hex strings are upstream commit ids and sha256 values of stock files (the oriole `fw_bcmdhd.bin` and `aoc.bin`), not content. `token 0xeb1c14e3` is the NVRAM length token (~words << 16 | words for 21388 B), not a secret. `logs/` is in `.gitignore` and `git ls-files logs` is empty [observed].
- `git ls-remote fork` [observed]: `main` 04457e5, `ci/xcprobe` cf7d4bb, `wip/oriole-next` 14c9780, `wip/oriole-touch-0145` 58ac758, tag `recovery-oriole-usbadb-1` 04457e5. Only `wip/oriole-next` moved in this run: reflog `update by push` c18dea7 at 23:36:27 (new branch) and 14c9780 at 00:45:14, a fast-forward (c18dea7 is an ancestor). `main`, `ci/xcprobe`, the tag (2026-10-26) and `wip/oriole-touch-0145` (last push 10:18:59, before START) are as before; nothing was deleted or forced. `wip/oriole-c4` was not pushed. `origin` was not pushed (no remote-tracking update). d8043cc and ebc8eaa are local only.
- `$W/pixos-lb-fhs.nix`: a buildFHSEnv definition, no secret; it holds the absolute path of the work disk (outside git).

## 5. Touch defaults and health: PASS for the defaults, FINDING R3 for the last GUI run

`git diff 8817998..ebc8eaa -- device/google/oriole/board.json` adds only the `fg` block and its note; the `xnu_bootargs_default` lines are context, unchanged; every run's boot-args end with the same five touch arguments.
`tools/test_mkadt_touch.py` has no diff [observed]. Last baseline run o2-base-14c9780 meets the criterion; the last GUI run o2-c4-batt1 lacks the scanning line (R3).

## 6. Patch numbers and boot-args: PASS (doc text stale, R5)

`../pixos-run.sh "tools/patch-numbers check"`: `RESULT: PASS this tree's patch numbers do not overlap other branches or reservations` [observed].
`tools/bootargs-known.txt:455` `sweetbatt patch:0148`, `:476` `sweetpcie1mmu patch:0149`; `docs/AUTOMATION.md:436` (sweetbatt section) and `:618` (sweetpcie1mmu section) [observed].
`validate`: `138 patches, series contiguous`.

## 7. Patch 0149 `sweet_pcie1_mmu_probe`: PASS (no reachable write, no unbounded loop), notes in R8

Read from source (`xnu/patches/0149-sweet-pcie1-mmu-probe.patch`, and `sweet_wlan.c` after 0124-0132 in `~/pixelios-artifacts/localbuild/work/c4`):
- Stores: none. The function body has three kinds of access, all loads: `cfg[0x04/4]`, `atu[((r << 9) | 0x100)/4 + i]`, `s[0]` and `s[0x2000/4]`. The only assignments are to locals. It calls `sw_map` (an `ml_io_map`, once per boot because `sweet_wlan_init` has a `started` latch) and `printf`. It does not call `sw_rd` (which can move the BAR0 window), `swl_wr` or `swl_store`.
- Loops: `r < 4`, `i < 7`, constants. No wait, no retry.
- Bounds: highest ATU offset 0x71C, inside the 0x1000 mapping of `SW_ATU`; S2MPU offsets 0 and 0x2000, inside the 0x4000 mapping. `cfg` and `atu` are non-NULL (checked before stage 1).
- Gating: `sweetpcie1mmu` must parse and equal 1, and the call sits inside `swl_stage1(...) == 0`, so only with the link in L0, ATU r0/r1 as the shim left them and the endpoint preconditions passed. Without the boot-arg the only new code is one `PE_parse_boot_argn`.
- Unsafe read? The EP Command/Status read is one config TLP through the window stage 1 has just used. The iATU reads are RC-local DBI. The S2MPU read is the risky one (R8 b); observed twice returning 0 with no fault. `FAULT_STATUS` is not known to me as read-to-clear (guess from memory of `s2mpu-regs.h`, where clearing is a write to INTERRUPT_CLEAR); patch 0102 reads the same two offsets on the G3D S2MPUs.

## 8. Open findings of earlier reviews listed in the report: PASS

The report lists WLAN F2, F4, F6-F9 as open and F1, F3, F5 as closed by 0147; 0147 N1-N5 (N4 = carried F2, M process); 0148 F1-F7 (all L).
The five N rows (`0147-...review-fable-1.md:129-133`) and the seven F rows of the 0148 review match that list [observed]. Of these, 0148 F5 (failure-time INT_STATUS/TRANS/FIFO
not printed) is now the blocker for understanding o2-c4-batt1, and 0148 F7 became real (R9).
