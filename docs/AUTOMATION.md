# Automation scripts (xrun, ci-kernel, stack-apply)

Three scripts replace the by-hand test loop. Each prints ONE report at the end (about 40 lines, no progress spam).
Line 1 is always `RESULT: <PASS|FAIL|PARTIAL|ERROR> <one sentence>`; read that first. Progress goes to a log file whose
path the report prints. Exit codes: **0** PASS, **1** FAIL or PARTIAL, **2** ERROR (the tool could not do its job:
no device, no network, bad arguments). `tools/pix xrun|ci|stack` are wrappers.

Evidence tags: everything in these reports is *observed* by the script on this run (camera, USB, log text, CI);
what a result *means* for the port is still the driver's judgement.

## tools/xrun: one hardware cycle

```
tools/xrun --kernel K [--adt A | --board bluejay] [--cpus N] [--qr S|off] [--restart S] [--args "boot-args"] [--rw] [--gui] [--usbnet [--usbdma]]
           [--expect REGEX]... [--expect-absent REGEX]... [--duration S] [--label NAME] [--no-flash-check]
           [--early] [--stop-on REGEX]... [--hard-timeout S] [--reader "qr-log.py options"]
tools/xrun --dry-run --from-dir logs/runs/<dir> [--stop-on REGEX]...   # rebuild the report of an old run, no phone (copy the dir first: report.txt/merged.log are rewritten)
```

Early stop (*read from source*, not yet run on hardware): `--stop-on REGEX` (repeatable; `--early` = all `--expect` patterns) ends the capture as
soon as every pattern is in the merged QR/usbnet log (checked every 10 s), instead of waiting for the self-reset. Then, if the phone is on USB (18d1)
xrun runs `to-fastboot.sh 60`; if not, the report gets ONE line `PHONE: still running; self-reset due in ~Ns / Power+VolDown hold`. RESULT is PASS when the
expects matched, with `EARLY: stopped at +Ns (full run would have waited Ms)`. A dry-run replay with `--stop-on` truncates the log at the moment the last
pattern first appeared and prints `EARLY: NOT reached ...` when it never would have. `--hard-timeout S` caps the whole run from xrun start (default: none, `--duration` still
applies); a timeout FAIL says `HARD TIMEOUT` and carries `PHONE: usb ids [...] adb [...] fastboot product [...]` from a bounded probe (lsusb 5 s, adb 8 s, fastboot 8 s).

Cycle: `to-fastboot.sh` (ERROR `needs Power+VolDown hold` if the phone is not reachable, ERROR if it is not bluejay) ->
`mkadt.py --board` -> `build-xnu-image.sh` -> camera/QR capture for the whole run -> `fboot.sh --expect xnu` -> waits on
its own until the phone re-appears on USB (vendor 18d1; the self-reset worked) or `--duration` (default restart+120 s)
runs out -> report. `--no-flash-check` skips the to-fastboot step (phone already in fastboot).
`--usbnet`: boot-args `sweetusb=2` (`--usbdma` adds `sweetusbdma=1`), xrun starts `tools/usbnet-log.py` (UDP 5140, no root) and prints the one sudo line
(`tools/sweet-usbnet.sh --stage`) the user runs once the phone enumerates; the report merges `usbnet.log` with the QR log (docs/research/09).
Everything lands in `logs/runs/<UTC>-<label>/` (gitignored): `report.txt`, `report.json`, `merged.log`, `snaps/` (raw QR
snapshots), `frames/` (every 4th camera frame + the last 20), `run.json`, `A.adt`, `image.meta.json`, `xrun.log` (progress).

Sections, fixed order:

| Section | Read it as |
|---|---|
| `RESULT` | PASS = every `--expect` found, every `--expect-absent` absent, no panic marker, reset observed (if `--restart`). FAIL = one of those broke, with a log that starts at boot and has no gaps. PARTIAL = an `--expect` is missing but the log is incomplete (the line may be outside the snapshots) or there is no QR log at all |
| `RUN` | kernel sha256, git head, boot-args, cpus/qr/restart; boot -> first QR snapshot; `reset observed at +Ns` or `NOT observed`; **log coverage** (snapshots, gaps, first/last line) |
| `EXPECT` | per regex: FOUND with the first matching line and its time (the snapshot that first contained it, seconds after the boot command), or MISSING |
| `MARKERS` | built-in counters (restart armed, UFS errors, display on, panic, lockscreen, SpringBoard/backboardd lines, check-in timeouts), FB swap count, `SWEET TOUCH/KEYS` lines (max 12), `SWEET CPU` lines |
| `CRASHES` | per process: count, signal, first pc, reason (from `CRASH <proc>[pid] sig N`) |
| `SCREEN` | mean camera luma per frame as segments (`bright 0-95 s, black from 97 s`) and `black-at`. No ML: mean luma < 25 = black |
| `DIFF` | markers/crashes that appeared or disappeared since the previous run with the same `--label` |

Limits you must know:
- **QL2** (kernel default, `sweetqr_fmt=2`, docs/research/24): the whole log from its first byte, compressed, loss-tolerant; the reader keeps
  `snaps/qr2-<sid>.txt` + `.json` (the log so far, `[...gap...]` where parts have not arrived yet). The report's "log coverage" line says
  `QL2 1 session(s): N of M reduced bytes recovered, blocks, frames, gaps`; with gaps an `--expect` that is missing is PARTIAL, not FAIL (the line may be in a gap). Wait longer
  (`--duration`) or use `--stop-on` to let more repair frames arrive. Camera/decoder knobs go in `--reader "--workers 3 --tiles 2 --average 3"`, phone-side
  knobs in `--args "sweetqr_ver=12 sweetqr_ms=500 ..."` (docs/QR_LOG.md). QL1 (`--args sweetqr_fmt=1`): only the **tail** (last 12 kB, digit-only-different
  repeats dropped), one snapshot per ~48 s; early/mid-boot lines are lost there.
  A run that must see lines while the phone is still alive has to start the QR takeover before that moment (`--qr SECONDS` counts from the root
  mount; QL2 reads the log from the start in the background, so a late takeover does not lose early lines). With `--qr off` there is no log: only reset and screen
  evidence, so `--expect` gives PARTIAL. Crash counts are lower bounds when the log has gaps.
- QR takes over the screen, so SCREEN `black`/`bright` during a QR run says little about the GUI.
- Times are relative to the `fastboot boot` command; snapshot time = when the camera finished decoding it (a few seconds
  after the phone drew it).

## tools/ci-kernel: the kernel for a commit

```
tools/ci-kernel [--commit SHA|HEAD] [--dispatch] [--expect-string STR]... [--out DIR] [--wait MIN]
```
Finds the `xnu.yml` run for the commit (waits up to 25 min), downloads `xnu-kernel` to `~/pixelios-artifacts/xnu-<short>/`
(reuses an intact earlier download), checks `SHA256SUMS`, runs `strings -a` for each `--expect-string`, prunes
`xnu-<7 hex>` dirs beyond the 3 newest (not with `--out`; dirs named `xnu-0089` are never touched). A failed CI run is
FAIL plus the last 25 lines of the failing step. Report: RESULT, RUN (id, url), KERNEL (path, sha, size), one line per string, PRUNED.

## tools/stack-apply: do the patches apply?

```
tools/stack-apply [--only-new PATCH] [--keep]
```
Applies the three stagers and every `xnu/patches/*.patch` in order on a pristine `xnu-8796.141.3` (shallow clone cached in
`~/pixelios-artifacts/xnu-pristine`, scratch copy under `~/pixelios-artifacts/scratch/`, deleted afterwards). Takes about 10 s plus the
stager downloads. FAIL names the first failing patch and prints 10 lines of the `git apply` error. `--only-new 0088`
still applies everything but states the verdict for that patch. It does not build; CI does.

## Dev overlay: change files in the flashed root without re-flashing (xnu patch 0117, reviewed in `docs/reviews/18-20-dev-overlay-*.md` (kernel clone) and `0117-dev-overlay.md` (super repo), never run on hardware)

```
tools/xrun --kernel K --overlay DIR [--args "..."] ...          # or: tools/build-xnu-image.sh --overlay DIR
tools/mkdevoverlay.py DIR OUT.bin [--owner UID:GID]               # the packer alone (default owner 0:0)
```
`DIR/usr/lib/libsweetgl.dylib` becomes `/usr/lib/libsweetgl.dylib` on the phone. The image gets the archive as the payload
RAM disk (`mkpayload.py --ramdisk`) and ` sweetdev=1` in the boot-args (607-byte budget still enforced by mkpayload). Before launchd starts,
`sweet_devoverlay_run()` (called in `bsd_init` after `vfs_mountroot`) copies the files into the mounted root (`rootrw=1`, i.e. inside `userdata`
only). Without `sweetdev=` it returns before printing anything; the payload of a normal `xrun` has no RAM disk.

* **Archive** (little-endian, documented in the header of the patch's `bsd/dev/sweet_devoverlay.c`, validated by the kernel and by `mkdevoverlay.parse`):
  `"SWEETDEV" | u32 version=1 | u32 count` then per entry (16-byte aligned) `u16 pathlen | u16 mode | u32 uid | u32 gid | u32 crc32 | u64 size | path | pad | data | pad`.
  Every length is checked against the remaining bytes before use; a structural error means nothing is written; a bad crc skips that file.
* **Allowlist** (anything else: packer error; kernel `SWEET DEV: refused PATH (reason)`): `/usr/lib/`, `/usr/local/`, `/private/var/dev/`,
  exactly `/System/Library/xpc/launchd.plist`, `/System/Library/LaunchDaemons/org.pixelios.*` (one file name). Also refused: relative paths,
  `..`, `.`, empty components, components over 245 bytes, control bytes, mode above 0777 (no setuid), names ending `.sweettmp`/`.sweetorig` (any case), symlinks and non-regular files in DIR, more than 40 MiB in total (the 64 MiB boot image holds ~16 MiB already).
* **Safety model:** every existing parent component is looked up without following symlinks and must be a directory; directories are created
  only at or below an allowlisted directory prefix; the target must be absent or a regular file. Per file: identical (size + bytes) -> `skipped`;
  else write `PATH.sweettmp`, set mode/uid/gid, fsync, rename over the target; the first replaced original is kept as `PATH.sweetorig`.
  Errors are logged and the loop goes on; no panics, all loops bounded (count <= 4096).
* **`sweetdev=restore`:** `--overlay DIR --args "sweetdev=restore"` (the overlay still names the paths) renames `PATH.sweetorig` back over `PATH` for every archive entry.
* **Log lines** (UDP log): `SWEET DEV: wrote|skipped|refused|failed|restored PATH ... N ms` per file, then `SWEET DEV: sync error N` (only on a VFS_SYNC failure), then `SWEET DEV: done: W written, S skipped, R refused/failed, T ms`.
* **First-run checklist** (review 18, `docs/reviews/18-dev-overlay-0117.md`). Preconditions: host tests below pass; the flashed root has finished its first (DataMigration) boot;
  the phone lock is held by you and the agent that owns the installed root has been told it gets ONE new file. **Never replace an existing file on the first runs.**
  1. `DIR/private/var/dev/hello` = `hello\n` (6 bytes), nothing else. `tools/xrun --kernel K --overlay DIR --label dev1 --restart 600 --expect "SWEET DEV: wrote /private/var/dev/hello 6 bytes" --expect "SWEET DEV: done: 1 written, 0 skipped, 0 refused/failed" --fail-on "SWEET DEV: (refused|failed|sync error|bad archive|sweetdev=.* but no SWEETDEV|root is not mounted|out of memory)" --fail-on "mounting dirty non-journaled root" --fail-on "panic|Kernel trap"`; add `--gui` plus the owning agent's GUI `--expect` so the GUI-marker PASS rule below is encoded in the command, not only read by hand.
     PASS = both expects, no fail-on, and the run still reaches the usual GUI marker of that agent's runs; the shim/loader lines must not say `ramdisk overlaps kernel memory`.
  2. Same image again: expect `SWEET DEV: skipped /private/var/dev/hello 6 bytes (identical)` and `done: 0 written, 1 skipped` (proves the read-back path and that run 1 reached the disk).
  3. Only after 1 and 2: a replacement under `/usr/local/` (not `/usr/lib/`), then a `--args "sweetdev=restore"` run for it. Size ramp for the RAM disk (research 41 U1) before anything over ~20 MiB.
  * **Recovery:** on any `panic`/`Kernel trap` in 1 or 2, boot the previous image WITHOUT `--overlay` (the hook is inert; HFS replays its journal). If that boot does not reach the GUI the root is damaged: stop,
    tell the agent that owns the image, and re-flash it with `tools/fflash-userdata.sh IMG --yes-erase-userdata` after it confirms which image. A bad `launchd.plist`: `--overlay DIR --args "sweetdev=restore"`
    (restores `.sweetorig`), else re-flash. Files the overlay CREATED (and created directories) are not removed by `restore`.
  * **Known limits** (review 0117 F3-F5, read from source, not fixed in code): (F3) replacing an existing file is two renames (`path` -> `.sweetorig`, then tmp -> `path`), not atomic: a UFS timeout, panic or reset between them leaves `path` absent and the original only at `.sweetorig`; the hook runs before launchd, so boot the SAME `DIR` and kernel with `--args "sweetdev=restore"` (or the same overlay again) to repair it. (F4) a file the overlay created has no `.sweetorig`; if its content changes in a later run, the previous overlay version becomes `PATH.sweetorig` and `restore` puts that version back (not "absent"): change a created file only via a new path name, or accept that `.sweetorig` holds the previous overlay version. (F5) "identical" compares data only, not mode or owner: a file with the right bytes but another mode/uid/gid is `skipped`; use a new file name to change them, and use the default `--owner 0:0` on first runs. (F1) `--overlay` together with `--args ufsroot=...` is refused by the build tool (the hook writes whatever partition `ufsroot` names). Never run fsck or write to the root from the host.
* **Tests:** `python3 tools/test_mkdevoverlay.py`, `python3 tools/test_build_overlay_args.py`, `python3 tools/test_devoverlay_kernel.py` (compiles the kernel's parsing/path functions on the host with ASan/UBSan, differential vs the packer's reader, plus mutants; skipped without gcc). Design and unknowns: kernel `docs/research/41-dev-overlay-without-reflash.md`.

## tools/phone-state: what state is the Pixel in?

```
tools/phone-state [--expect android|recovery|fastboot|none] [--wait S]   (also: tools/pix phone)
tools/phone-state --self-test     # stub lsusb/adb/fastboot on a fake PATH, incl. a hanging adb; run by check-all.sh
```
Every probe runs under `timeout` (8 s max; a bare `fastboot getvar` against a phone in recovery hangs 120 s). Order: `lsusb` for
18d1; `adb devices` + `getprop ro.product.device`/`sys.boot_completed`; `fastboot devices` + `getvar product` only when the usb id
is 18d1:d001 or 18d1:4ee0 (guess: other tools only assume vendor 18d1). Any non-bluejay device is `state=other`
(`OTHER-DEVICE (not touched)`). Last line: `RESULT: PASS|FAIL|ERROR state=fastboot|recovery|android|booting|absent|other product=bluejay|unknown`,
exit 0/1/2; ERROR = a probe timed out. Without `--expect`, PASS means "bluejay present". `--wait S` re-probes every 2 s (cap 300).
Serials are never printed. `to-fastboot.sh`, `fboot.sh` and `xrun` do not use it yet (own refusal/loop logic; swapping is not trivial).
Verified with stubs only (self-test), not on hardware.
## tools/tool-proposals: the proposal register

```
tools/tool-proposals list [--index]            # id, tool, status; --index = the table in docs/TOOL-PROPOSALS.md
tools/tool-proposals check NAME_OR_KEYWORD     # also: tools/pix proposals ...
tools/tool-proposals new NAME [--extends X] [--related TP-NNN] [--force]
tools/tool-proposals validate
```
`check` searches `tools/` file names, `tools/pix` subcommands, the headings of this file and the `tool`/`usage` fields of every proposal; one line
per hit (`EXISTS tools/x`, `PROPOSED TP-NNN (status)`), last line `RESULT: EXISTS|ALREADY-PROPOSED|NONE ...`, exit 0 NONE / 1 hit / 2 error.
`new` refuses on a hit unless `--force`, writes the next free `docs/tool-proposals/TP-NNN-slug.md` from the skill template and prints the
path; it does not edit the index, and the placeholders make `validate` fail until they are filled. `validate` checks fields, status values,
ids (unique, contiguous, match the file name), `extends`/`related` targets, the five sections, and that the README index equals
`list --index` (an `(amended)` suffix on the status is generated from a non-empty `## amendments`). Last line `RESULT: PASS|FAIL ...`, up to 10
problems, exit 0/1/2. `tools/validate-repo.py` runs it, so the gate fails on a broken register. Unit-tested (`tools/test_tool_proposals.py`).

## tools/hang-recover: wait for the phone after a reboot, bounded (TP-006)

```
tools/hang-recover [--wait S] [--expect fastboot|recovery]   (default 90 s, cap 300; also: tools/pix recover)
tools/hang-recover --self-test    # stub lsusb/adb/fastboot; run by check-all.sh (hang-recover step)
```
*Observed*: after a shim probe or an XNU run the phone leaves USB for seconds to a minute while it reboots, then shows up as
recovery; an early "hang" verdict was wrong. So: loop until the deadline, `tools/phone-state` every 3 s (never a bare adb/fastboot),
one line per state change (`+12s recovery`). Seeing recovery/android with `--expect fastboot` runs `tools/to-fastboot.sh` once
(bounded 60 s). Last line `RESULT: PASS state=fastboot after Ns` (exit 0), or `RESULT: FAIL still absent after Ns` (exit 1) followed by
"Hold Power+VolDown ~10 s until the bootloader screen / fastboot shows, then rerun tools/hang-recover" and "if the screen shows a
bootloader menu but USB is absent, replug the cable". A non-bluejay device = exit 2, not touched. Software only: the relay/hardware
reset half of TP-006 is not built (open for the user's decision). Verified with stubs only (self-test), not on hardware.

## tools/check-all.sh: the quality gate

`tools/check-all.sh` (full) is what must print `GATE PASS <hash>` before a push. Output is one line; `shim/out/gate.log` has the
full log (steps in a fixed order) plus a per-step timing table. *Read from source* (`tools/check-all.sh`, this branch; not yet measured on a full gate):

- **Lanes.** Two lanes run in parallel: lane A = shim builds + host-tests + boot-safety, lane B = validate, public-split, unit, phone-state,
  hang-recover, probe-run, rootless. Lane A runs `build` first and serially (it creates `shim/out/bluejay-testkey.pem`, warms the audit
  memo and the host-test stamp, and forces the host tests: `PIXELIOS_HOST_TESTS=always`), then the other 11 build steps in parallel,
  at most `GATE_JOBS` (default 8; `-j N` works too) at a time, then host-tests (the binaries of `build`) and boot-safety (`--out shim/out/gate/build-0`).
- **No shim/out writes, no `.build.lock`.** Every `build.sh` call gets its own dir `shim/out/gate/<step>-<i>/` (`--out`, below); the gate
  never overwrites `shim/out/*.img` and does not wait for xrun/probe-run. It takes `shim/out/.gate.lock` instead (one gate per worktree;
  `tools-sync pull` refuses while it is held). Per-step logs: `shim/out/gate/<step>.log` (live); `gate.log` is rebuilt from them at the end.
- **Live state.** `shim/out/gate.progress` (`START <step> <epoch>` / `END <step> PASS|FAIL [(timeout)] <s>` lines), `shim/out/gate.result`
  (atomic: `WAITING <pid> <epoch>` while the gate waits for `.gate.lock` (it replaces the old result; a lock timeout writes `GATE FAIL lock-timeout`),
  `RUNNING <pid> <epoch>`, then the final `GATE ...` line + epoch). The waiter names the holder from `shim/out/.gate.owner` (removed when a gate ends; `unknown` if empty).
  A hash with `+dirty` means `git status --porcelain` was not empty at the start, or HEAD moved / the tree changed during the run (read from source: tools/check-all.sh, tools/test_check_all_stub.sh).
  The stamp is `HEAD` taken at the start, written only by a full (or quick-turned-full) PASS whose HEAD and tree did not change; never after a quick run or any failure.
- **Interrupt (Ctrl-C, kill).** INT/TERM/HUP kills every step's process group (`timeout` puts each step in its own group, pids in a temp dir), the lane jobs,
  releases the lock, and writes `GATE FAIL <hash> interrupted <epoch>` to `gate.result` (also printed). A lane never starts a step once the gate is gone or interrupted.
  Tested with stubs: `bash tools/test_check_all_stub.sh` (~15 s, also run by the `unit` step through `tools/test_check_all_stub.py`).
- **Timeouts and failure.** Each step runs under `timeout -k 10`: 1500 s for `build-wlan`, 900 s other `build*`, 300 s others; `GATE_STEP_TIMEOUT=SECONDS` (positive integer; `0`, empty-digits or non-numeric values exit 2, since `timeout 0` means no limit) overrides all;
  a timeout is a step FAIL `(timeout)`. A failing step stops every other step at its next step boundary (running parallel builds finish);
  `--keep-going` stops only the failing lane. The reported failure is the first failed step in the fixed step order.
- **shellcheck step (lane B).** `shellcheck -S warning $(tools/shell-files)`, zero tolerance, over every tracked `*.sh` or sh/bash/dash-shebang script
  (`tools/shell-files` is the single listing; it also lists untracked-but-not-ignored scripts, so a new script is linted before `git add`). `--quick` selects it for any changed shell file (`gate_select.is_shell`). Fix findings; `# shellcheck disable=SCxxxx` needs a reason on the same line.
  `shim/build.sh` carries two such directives (CFLAGS word splitting, host-test hash file lists). Needs `shellcheck` installed (apt).
- **Flags and env.** `--quick`, `--keep-going`, `--no-memo` (= `PIXELIOS_AUDIT_NO_MEMO=1`; `1`, `true`, `yes` in any case turn the memo off; `0`, `false`, `no`, `off` or empty keep it on; any other spelling prints a warning on stderr and also turns it off, fail safe), `-j N` (= `GATE_JOBS=N`), `GATE_STEP_TIMEOUT`.

**Audit memo (`tools/audit_memo.py`, R2).** The three expensive mutation self-tests of `tools/test-bluejay-boot.py` (`wlan_seq_selftest`,
`wlanlink_source_selftest`, `wlanlink_object_selftest`) are cached by content. Key = sha256 of the audit script bytes, `tools/audit_memo.py` itself, the function name,
the argument bytes (the objdump text for the object self-test; objdump itself stays outside), `sys.version` + implementation; the entry also
records every file the function opened (`sys.addaudithook`, event `open`) with its sha256 taken at open time and re-hashes them on lookup. Only a PASS is stored, and only if every recorded file still has its open-time hash after the run (an edit during the run stores nothing);
a run that started a process or wrote a file is never stored; a corrupt entry is a miss. A hit prints the captured lines with
`(memo <key16>)` appended. The audit of the real objects/sources (`check_*` on `shim/out/*.o`, `shim/*.c`) is never memoized.
Cache dir: `$PIXELIOS_AUDIT_MEMO_DIR`, else `${XDG_CACHE_HOME:-~/.cache}/pixelios/audit/` (shared by worktrees; entries store repo paths relative
to the repo root). Off: `tools/test-bluejay-boot.py --no-memo`, `PIXELIOS_AUDIT_NO_MEMO=1`, `check-all.sh --no-memo`.
Tests: `tools/test_audit_memo.py` (key parts, dep tracking, FAIL / subprocess / write not stored, corrupt entry, replay) and
(path normalisation across worktrees, mid-run edit) and `tools/test_audit_memo_mutations.sh` (also run by the `unit` step through `tools/test_audit_memo_mutations.py`, ~30 s; ends `RESULT: PASS mutants=N caught=N`; runs from a temp copy with a warm memo and requires each
mutant of the wlanlink source, the audit script, `wlan_seq.json` and `boards.py` to still FAIL; the object self-test needs an aarch64
objdump and a built `wlanlink.o` and is not covered by it; logs in `logs/audit-memo-mut/<UTC>/`).

**`shim/build.sh` additions.** `--out DIR` (default `out`; relative to `shim/`; `[A-Za-z0-9._/-]` only) replaces every `out/` path; the AVB test key
stays `out/bluejay-testkey.pem`. `tools/test-bluejay-boot.py --out DIR` audits that dir (default `shim/out`) and takes one or more boot
images (header checks per image, the tree audit once). The variant-independent host tests (and the extracted xnu-patch headers) run once per input digest:
sha256 of `shim/*.c *.h`, `out/board_config.h`, `out/wlan_seq.h`, the `../xnu/patches/NNNN` files `build.sh` references, `gcc -v` and `build.sh`;
the stamp `${XDG_CACHE_HOME:-~/.cache}/pixelios/host-tests/<digest>.ok` is written only after all of them passed
(`host tests: cached <digest>` otherwise; `PIXELIOS_HOST_TESTS=always` runs them anyway); the digest is recomputed after the tests and no stamp is written if an input changed meanwhile.
Every host-test compile and run goes through `chk` (a failure stops the build; no stamp), and `chk` lists each compiled `*host_test*` output in `$O/host-tests.list`.
The gate's `host-tests` step runs exactly that list from `out/gate/build-0` and FAILS on a missing list or binary (no silent skip); `host_test` writes its `.ppm` only to a path given as argv[1].
`tools/test_build_hosttests.sh` (via `test_build_hosttests.py`) tests this plumbing without a build, and that the `--wlan-iso --wlan-en` build is refused with the intended message. The variant `nm` checks always run.

`tools/check-all.sh --quick` is for iterating: it diffs the tree against the commit in `shim/out/gate.stamp` (written only by a
full PASS on a clean tree) and runs only the needed steps, selected by `tools/gate_select.py` (tested by `tools/test_gate_select.py`):

| Changed paths | Steps |
|---|---|
| `shim/`, `soc/`, `device/`, `tools/imgmeta*`, `tools/mk*`, `tools/test-bluejay-boot.py`, `tools/check-all.sh`, the xnu patches `shim/build.sh` references as `../xnu/patches/NNNN` (0087, 0088, 0093, 0104 today; the host tests extract headers from them; read from build.sh, not listed in code); `shim/*.md` selects no build | 6 builds, host-tests, boot-safety |
| `tools/`, `soc/`, `device/` | unit, phone-state, hang-recover, rootless |
| `tools/`, `soc/`, `device/` | unit, phone-state, probe-run, rootless |
| anything (docs, other `xnu/` patches) | validate (patch numbering/header format, script syntax), always |

No stamp, or a stamp that is not an ancestor of HEAD: the full gate runs. A quick PASS line says `quick: ran N of 17 steps`;
it is not a substitute for the full gate before a push. `tools/audit_memo.py` selects the build steps like `tools/test-bluejay-boot.py`.

## shim --gpu-power: Mali-G78 power on/off cycle (G2 stage 1, opt-in)

```
cd shim && ./build.sh --board bluejay --avb --pstore-fast --gpu-probe --gpu-power     # --gpu-power implies --gpu-probe
tools/probe-run --build-args "--pstore-fast --gpu-probe --gpu-power" --expect "GPU POWER up=" --label g2   # only with the user's go-ahead (writes hardware)
```
Source `shim/gpupower.c/.h`, host test `shim/gpupower_host_test.c` (run by `build.sh`), constants from `soc.json` key `gpu` (`mali`, `id_expect`, `shader_present`,
`tiler_present`, `l2_present`, pinned by `tools/boards.py _GPU_PINS`). `gpupower.o` is compiled and audited on every build (source audit with 26 mutation self-tests, object audit with 3, in
`tools/test-bluejay-boot.py`) and linked only with the flag; the default image is unchanged. The call runs after the read-only probe output (section `GPU`). *Read from source*: sequence = Google kbase JM
(`docs/research/27-gpu-g0-result.md` addendum has the file:line table and the steps deliberately omitted). *Not yet run on hardware.*
- **Refusal (zero writes)** unless the gate is open, the timer works, `GPU_ID` and `SHADER/TILER/L2_PRESENT` equal the soc.json values, their HI words and all `*_READY` / `*_PWRTRANS` (LO and HI) are 0,
  `GPU_STATUS` = 0, `GPU_FAULTSTATUS` = 0 and `GPU_IRQ_RAWSTAT` = 0x100 exactly:
  `GPU POWER refused: <pd_g3d-off|pd_embedded_g3d-off|cmu-mux-off|pmu-unreadable|cmu-unreadable|no-timer|id-unreadable|id-mismatch|present-unreadable|present-mismatch|present-hi-nonzero|core-state-unreadable|cores-not-off|gpu-state-unreadable|gpu-status-nonzero|gpu-fault|rawstat-unexpected>`.
- Read-only pre-state lines `GPU PRE_<STACK_READY|STACK_PWRTRANS|L2_CONFIG|COHERENCY_ENABLE|GPU_IRQ_MASK> pa=... v=...`.
- ON: `TILER_PWRON` (also powers the L2), wait tiler+L2 ready, `SHADER_PWRON`, wait shaders ready. Then `GPU <NAME> pa=... v=...` for `SHADER/TILER/L2_READY`, the three
  `*_PWRTRANS`, `GPU_IRQ_RAWSTAT`, `GPU_FAULTSTATUS`, `GPU_STATUS`. OFF, always (also after an ON failure), per block as kbase: wait PWRTRANS 0, **write only if READY != 0**, refuse a partial READY, wait;
  shaders (`SHADER_PWROFF`) then L2 (`L2_PWROFF`, takes the tiler too); the same dump with the prefix `OFF_`. Each wait is bounded by 100000 reads and 1 s of CNTPCT; no write is retried.
  If the shaders do not power off, `L2_PWROFF` is not written.
- Summary: `GPU POWER up=<what> down=<what> ticks=<ON phase>/<OFF phase> freq=<Hz>` (hex, CNTPCT ticks). `up`: `ok|timeout:tiler|timeout:l2|timeout:shader|fault:tiler|fault:shader|refused:tiler|refused:shader`;
  `down`: `ok|timeout:shader|timeout:l2|timeout:tiler|partial:shader|partial:l2|fault:shader|fault:l2|refused:shader|refused:l2` (`fault:` = the write faulted on the bus, `refused:` = our allowlist refused it,
  `partial:` = READY is not the full present mask, nothing written). Success means `up=ok down=ok` and every `OFF_*_READY` = 00000000.
- Writes (the whole allowlist, LO words only): `0x1C500180` SHADER_PWRON, `0x1C500190` TILER_PWRON, `0x1C5001C0` SHADER_PWROFF, `0x1C5001E0` L2_PWROFF, each only with its present mask.
  Never: `L2_PWRON`, `TILER_PWROFF`, `STACK_*`, `GPU_COMMAND`, reset, MMU/job registers, IRQ masks, PMU/CMU/SMC/ACPM.

### `--gpu-power=stack`: corestack variant (opt-in experiment, after the first run)

`./build.sh ... --gpu-probe --gpu-power=stack` (implies `--gpu-power`; plain `--gpu-power` is unchanged). Reason: in the first hardware run `SHADER_PWRON` left `SHADER_PWRTRANS` stuck
(docs/research/27 last section); kbase's `corestack_driver_control=true` path powers the core stacks first. *Read from source*: pm_driver.c:2593-2597 (`stacks_avail` = present),
2646-2649 (`STACK_PWRON`), 2656-2657 (wait `!stacks_trans && stacks_ready == avail`), 2658-2660 (then `SHADER_PWRON`); off 2846-2847, 2855-2859 (`STACK_PWROFF` after the shaders are off), 2866 (wait); registers
enums.h:727-731, selection pm_driver.c:408-424. Whether the Pixel build enables it is unknown (*guess*); this variant is the test.
- Object `shim/out/gpupower_stack.o` (`-DGPU_POWER_STACK`) is built and audited on every build; only this flag links it. Both objects have their own exact allowlist audit; the plain one must not contain the stack registers.
- Extra precheck (refusals, zero writes): `stack-present-mismatch` (`STACK_PRESENT` LO != soc.json `stack_present` 0x7F or HI != 0), `stack-not-off` (`STACK_READY` != 0), `stack-unreadable`.
- Order: `TILER_PWRON` -> wait tiler+L2 -> **`STACK_PWRON` (0x1C500E20) = 0x7F** -> wait `STACK_READY` (0xE10) == 0x7F and `STACK_PWRTRANS` (0xE40) == 0 -> `SHADER_PWRON` -> wait. OFF: `SHADER_PWROFF` -> **`STACK_PWROFF` (0x1C500E30) = 0x7F** (only if `STACK_READY` != 0) -> drain tiler PWRTRANS -> `L2_PWROFF`. A failure at any OFF step stops the later ones (no stack-off under ready shaders, no L2-off under a ready stack).
- New result words: `up=timeout:stack|fault:stack|refused:stack`, `down=timeout:stack|partial:stack|fault:stack|refused:stack`. The dump (`dump_all`, both variants) now includes `STACK_READY` / `STACK_PWRTRANS` (also `OFF_`).
- Allowlist of this variant: the four of plain `--gpu-power` plus exactly `0x1C500E20` and `0x1C500E30`, each only with 0x7F (6 pairs).

## shim --gpu-clk: ask the ACPM for the GPU clocks (G3DL2 / G3D), opt-in, before the power cycle (BUILT, UNRUN; hypothesis refuted by the PLL_CON3 decode, docs/research/27)

```
cd shim && ./build.sh --board bluejay --avb --pstore-fast --gpu-probe --gpu-clk [--gpu-power]     # implies --gpu-probe; works without --gpu-power
tools/probe-run --build-args "--pstore-fast --gpu-probe --gpu-clk --gpu-power" --expect "GPU CLK result" --label g2clk    # only with the user's go-ahead (writes hardware)
```
Status: **built, unrun, premise refuted.** It was written because run `g0b` seemed to show both GPU PLLs off; the correct decode of `PLL_CON3` = 0xA0620402 (ENABLE + LOCK set, 150.5 MHz = the 151000 kHz init level, docs/research/27) says they are already running, so a clock request is not expected to change anything. Kept as an opt-in experiment; open review findings (rate 302000/151000 as Linux, a meaningful result check, PMU gate, no power cycle after a failed request) are listed in doc 27. Original idea: the ACPM starts the PLLs when the kernel calls `exynos_acpm_set_rate(G3DL2/G3D)` (*guess*). Source `shim/gpuclk.c/.h`, host test `shim/gpuclk_host_test.c` (reuses the fake APM of `dvfs_host_test.c`; two more
builds force one ACPM gate to 0), constants from soc.json (`acpm.dvfs.domains` G3D idx 5 / G3DL2 idx 6 with the fvmap rows of run g0b and `ceiling_khz` = 151000 = the firmware's own init level; `gpu.clk_khz` = 151000),
checked by `tools/boards.py` (`tools/test_boards_gpuclk.py`). The CPU `--dvfs` requests and the touch probe still see only the three CPU clusters (`ACPM_DVFS_N` = 3; the acpm.c tables hold 5).
- **No new writer.** `gpuclk.c` has no MMIO access of its own (audited: no `try_write32`/`try_read32` reference, stack-only stores, 14 source mutations + 3 object mutations). Its only requests go through `acpm_dvfs_reassert()` in
  `acpm.c` (compiled only in `--gpu-clk` images), a thin wrapper of the existing DVFS path (`dvfs_set`, the same `xfer()` and writer allowlist as the CPU client): it refuses every domain but 5/6 and every rate but a table row at or
  below the ceiling, i.e. only 151000 kHz, before anything is sent. One difference from `acpm_dvfs_set`: it also sends the request when GET already reports that rate (Linux re-sends the init rate at every probe; the whole point).
- **Gates.** All existing ones apply and are checked first, with the reason printed and zero writes: DVFS channel configured, `acpm.layout_verified`, `acpm.elem_unit_verified` (both **1** in soc.json since 2026-10-22: descriptor fields and 16-byte slots
  observed on the PMIC channel, run `acpm3`; the DVFS channel 1 GET was observed on 3/3 CPU clusters in run `dvfsr2`), a validating descriptor, empty queues, a working timer.
  The unverified fact that remains: a FREQ_REQ for the GPU domains (idx 5/6) has never run, nor any GET for them (a CPU FREQ_REQ through this client did run: `dvfs1..4`, effect observed, reply words not logged) and nothing is known about the reply for the GPU domains
  (Linux sets `response=true` for them, acpm_dvfs.c:97-135; our client waits for the reply for every request anyway).
- **Order** (kbase `gpu_dvfs_set_initial_level`, pixel_gpu_dvfs.c:859-896; domains from gs101-gpu.dtsi:46-47): GET G3DL2, GET G3D, then FREQ_REQ G3DL2 = 151000, FREQ_REQ G3D = 151000 (a failure of the first stops the second, no retry), ~10 ms, GET both back,
  `GPU CLK after: ...` and the CMU_G3D dump again (the `--gpu-probe` dump is the "before").
- **Output** (`GPU CLK ...`): `cfg dvfs_ch=.. layout_verified=.. elem_unit_verified=..`, `target kHz=151000 g3dl2_idx=6 g3d_idx=5`, `desc/queues` lines, per transaction `<get|pre-read|request> <G3DL2|G3D> req=.. / rep=.. / seq= err= ret= val= writes= pub=`,
  `G3DL2 kHz=<n>`, `readback g3dl2=<n> g3d=<n> asked=151000`, `result: ok|MISMATCH|REFUSED-or-FAILED`, refusals as `refused: <reason>` (+ `flag: soc.json acpm.<flag> = 0` for a gate). A request that was published and failed prints `STATE` and `the request stays queued`.
- **What can be written** (exactly): a run is 8 ACPM transactions on channel 1 (2 GETs, per domain a pre-read GET and a FREQ_REQ, 2 read-back GETs = 6 GETs + 2 REQs); each one makes the same 8 writes of acpm.c's audited writer (4 request-slot words, the request-front pointer, the doorbell bit, and after the reply the response rear pointer and INTCR0); whatever the firmware then does to its PLLs/PMIC is outside our code. Nothing else.

## shim --gpu-rails: READ-ONLY S2MPG11 registers of the GPU rails (opt-in, implies --gpu-probe)

```
cd shim && ./build.sh --board bluejay --avb --pstore-fast --gpu-probe --gpu-rails
tools/probe-run --build-args "--pstore-fast --gpu-probe --gpu-rails" --expect "GPU RAIL result" --label g0rails
```
Why (*guess*, doc 27): the tiler/L2 (rail BUCK8S = S8S_VDD_G3D_L2) came up but the shaders (rail BUCK2S = S2S_VDD_G3D) never leave PWRTRANS; the DT initial mode of all three GPU rails is SEC_OPMODE_SUSPEND ("ON in standby"). This reads what the PMIC says.
Source `shim/gpurails.c/.h`, host test `shim/gpurails_host_test.c` (fake APM of `acpm_host_test.c`, plus gate-0 and null-config builds), config `soc.json` `acpm.s2mpg11` (pins in `tools/boards.py _ACPM_S11_PINS/_ACPM_S11_REGS`, tests `tools/test_boards_gpuclk.py`).
- **Derived the same way as S2MPG10's speedy_ch/pmic_addr** (doc 23 section 1): channel = 1 (every `s2mpg11_*_reg` passes `u8 channel = 1`, s2mpg11-core.c:55,73,91,109,127; DT `acpm-speedy-channel = <1>`, gs101-pmic.dtsi:710-718), TYPE = pmic_addr = 1 (`I2C_ADDR_PMIC`, s2mpg11-core.c:29), IPC channel unchanged (2). Request word 0 = `reg | 1 << 8 | 1 << 12 | seq << 16`, word 1 = 0 (FUNC_READ).
  The registers (s2mpg11-register.h:26-100, regulator descriptors s2mpg11-regulator.c:226-275) are listed with their cites in the soc.json `s2mpg11` note. Any null value in the block => `GPU RAIL refused: ... null/unknown value, no ACPM request sent` and zero ACPM traffic.
- **Reuse, no fork**: `acpm_s11_read()` in acpm.c is a 15-line twin of the S2MPG10 `read1` and calls the same `xfer()`/writer allowlist; `gpurails.c` has no MMIO access of its own (audited: no `try_write32`/`try_read32`, stack-only stores; 13 source + 3 object mutations).
- **Gates / refusals (zero ACPM traffic)**: `acpm.layout_verified`, `acpm.elem_unit_verified` (both 1), a working timer, a validating PMIC-channel descriptor, empty queues, a fully known s2mpg11 block.
- **Output**: `GPU RAIL cfg speedy ch=1 pmic_addr=1`, then per register `GPU RAIL <STATUS1|B2S_CTRL|B2S_OUT1|B2S_OUT2|B8S_CTRL|B8S_OUT1|B8S_OUT2|L1S_CTRL1|L1S_CTRL2|LDO_CTRL1> reg=0x.. v=0x..`, then `GPU RAIL decode <rail> mode=<0-3>(name) enabled=<0|1> uV=<n>` for BUCK2S/BUCK8S/LDO1S and `decode note:` (mode field and uV formulas *read from source*; mode 1 = SUSPEND is the DT default of the CPU rails too, so it is not evidence of standby; that OUT1 is the live output is a *guess*).
- **Exactly the ACPM traffic**: one descriptor read (reads only), then at most **ten** FUNC_READ transactions on IPC channel 2 (S2MPG11 registers 0x0C, 0x15, 0x16, 0x17, 0x25, 0x26, 0x27, 0x41, 0x42, 0x52, in that order), each making the audited IPC writes of acpm.c (4 request-slot words, the request-front pointer, the AP->APM doorbell bit, the reply-rear pointer, INTCR0). The first failure stops the rest, no retry, nothing rolled back. No PMIC register is written (no FUNC_WRITE/UPDATE is expressible in this path).

## shim --gpu-rail-on: switch the shader rail on for the power cycle, and back (opt-in WRITER, implies --gpu-rails)

```
cd shim && ./build.sh --board bluejay --avb --pstore-fast --gpu-probe --gpu-rails --gpu-rail-on --gpu-power
tools/probe-run --build-args "--pstore-fast --gpu-probe --gpu-rails --gpu-rail-on --gpu-power" --expect "GPU RAILON" --label g2rail    # only with the user's go-ahead and after the Opus review
```
Why: run `g1rails2` (doc 27, *observed*) found S2MPG11 BUCK2S (S2S_VDD_G3D, the shader rail) OFF (CTRL 0x15 = 0x38, mode 0, OUT1 0x40 = 600000 uV) while BUCK8S (L2) and LDO1S were on: the shaders cannot power without their rail.
Source `shim/gpurailon.c/.h` (orchestration, no MMIO), the writer `acpm_s11_rail_set()` in `acpm.c` (a twin of `acpm_pmic_update`, same `xfer()` and IPC writer allowlist), host test `shim/gpurailon_host_test.c` (fake APM, gate-0 and null builds), config `soc.json acpm.s2mpg11.rail_on`
(pins `tools/boards.py _ACPM_S11_ON_PINS`, cites in the soc.json note, table of the write in doc 27).
- **Sequence** (shim.c, after `gpu_rails`, before the power cycle): (1) pre-read BUCK2S_CTRL and read OUT1; refuse with ZERO writes unless the mode field (bits 7:6) is exactly 0 and OUT1 decodes inside 550000..700000 uV (codes 56..80); (2) ONE FUNC_UPDATE of 0x15, mask 0xC0, value **0x40** (mode 1 = SUSPEND, what Linux writes for this rail, *read from source*: gs101-pmic.dtsi:766-779 initial mode + always-on, s2mpg11-regulator.c:35-91,718),
  never the voltage registers; (3) wait 8 x 130 us (the driver's `ENABLE_TIME_BUCK`), read CTRL, OUT1 and OUT2 back once, accept only if mode == 1 and OUT1 unchanged (`GPU RAIL on after ...`, `on result: rail is on, voltage unchanged`); (4) the power cycle runs only then (also `--gpu-clk` now skips it after a failed request);
  (5) ALWAYS afterwards: pre-read, ONE FUNC_UPDATE of 0x15, mask 0xC0, value 0x00 (only if the mode is not already 0), read back, `GPU RAILON enable=<ok|unverified|refused> readback=<ok|mismatch|n/a> restore=<ok|FAILED|none> power=<ran|skipped|not-requested>`; before the restore the shader state is logged (`GPU RESTORE_SHADER_READY` ...). A failed restore prints `GPU RAIL !!! RESTORE FAILED: ... may be LEFT ON` and `restore=FAILED`.
  After a refusal (nothing written) there is nothing to restore; after an enable that was published but not verified (error reply, mismatch) the power cycle is skipped and the restore still runs.
- **Gates (zero ACPM traffic, `GPU RAIL on refused: ...`)**: `acpm.layout_verified`, `acpm.elem_unit_verified` (both 1), a working timer, a validating PMIC-channel descriptor, empty queues, and a fully known `acpm.s2mpg11` + `rail_on` block (a null value refuses, `S11_ON_KNOWN` 0).
- **Exact writes of the option** (besides the IPC words every ACPM request makes, 8 per transaction): two PMIC register updates, listed with cites in doc 27. Transactions: enable = pre-read 0x15, read 0x16, update, read-back 0x15, 0x16 (5); restore = pre-read 0x15, update, read-back 0x15, 0x16 (4); one request per write, never retried.
- **Run order** (Opus review): (1) FIRST `--gpu-probe --gpu-rails --gpu-rail-on` ALONE (no `--gpu-power`): enable, read back, restore; expect `GPU RAILON enable=ok readback=ok restore=ok power=not-requested` and `on after ... mode=1`. (2) Only then add `--gpu-power`. Never combine with `--gpu-clk`.
  A rail left ON (hang mid-cycle, `restore=FAILED`) cannot be cleared by the shim: the next run's enable refuses (mode != 0) and does no restore; a SoC reset (PSCI) is not a PMIC reset, so S2MPG11 probably keeps the value (*guess*); since the value is mode 1 = 0x40 this equals the stock Android state (idle, gated cores: leakage only, *guess*).
- **Guess**: the soft-start margin (8x the driver's 130 us) is ours. B2S_CTRL bits 5:0 (0x38, vs 0x18 on BUCK8S) have no name in the header; the update preserves them, OUT2 is printed before/after.

## shim --gpu-s2mpu and --gpu-job: G2 stage 2 (address space 0 + the first job), design in docs/research/28-gpu-stage2-job.md

```
cd shim && ./build.sh --board bluejay --avb --pstore-fast --gpu-probe --gpu-s2mpu          # READ-ONLY: the four G3D S2MPU blocks. Safe to run first, alone.
cd shim && ./build.sh --board bluejay --avb --pstore-fast --gpu-probe --gpu-job            # implies --gpu-rails --gpu-rail-on --gpu-power --gpu-s2mpu; a WRITER (needs the Opus review and the user's go-ahead)
tools/probe-run --build-args "--pstore-fast --gpu-probe --gpu-s2mpu" --expect "GPU S2MPU3 vid1" --label g2s2mpu
tools/probe-run --build-args "--pstore-fast --gpu-probe --gpu-job" --expect "GPU JOB SUMMARY" --label g2job
```
- **`--gpu-s2mpu`** (`shim/gpus2mpu.c`, host test `gpus2mpu_host_test.c`, object audited like `gpuprobe.o`): prints `GPU S2MPU probe_pa=.. gb_index=..`, per instance `CTRL0/CTRL1/CFG/MPC_CTRL/PAGE_AWARE0/1/FAULT_STATUS`, per VID `FAULT_INFO/FAULT_PA_LOW`, `vidN rdwr=<mask> l2=<mask> nonzero= bad=` (64 granules of 1 GiB: which are open RD+WR, which use an L2 table) and the L1 entry of the granule of `probe_pa`.
  Only if `PMU_G3D_STAT` says `pd_g3d` is on; never writes. Config: soc.json `gpu.s2mpu_g3d0..3` (pinned in `tools/boards.py`).
- **`--gpu-job`** (`shim/gpujob.c`, hook in `gpupower.c`, host test `gpujob_host_test.c` with a fake Mali: AS0, page-table walker, WRITE_VALUE executor, fault / hang / S2MPU-block injections): inside the powered window it builds the page tables and one Valhall WRITE_VALUE job in 5 pages of the shim image (`gpujob_pages` in shim.c), programs AS0, submits on JS0, waits 100 ms
  (one `HARD_STOP_1` after a timeout), prints everything and always disables AS0 and clears the IRQ bits it caused; then the usual OFF sequence and rail restore. Output: `GPU JOB cfg`, the S2MPU dump, `GPU JOB <REG> pa= v=` lines, `GPU JOB target=<64-bit> header_exc=`, and
  `GPU JOB SUMMARY submit=<ok|no> wait=<done|failed|mmu-fault|timeout|n/a> hardstop=<yes|no> target=<MAGIC|unchanged|other> as0_off=<ok|FAILED>`; refusals `GPU JOB refused: <scratch-bad|no-timer|js0-absent|js0-no-set-value|shaders-not-ready|job-irq-pending|mmu-irq-pending|as0-active|js0-busy|gpu-fault>` with zero writes (`GPU JOB JS_PRESENT` and `JS0..2_FEATURES` are printed first, refused or not; slot 0 needs `JS_PRESENT` bit 0 and `JS0_FEATURES` bit 2 = SET_VALUE_JOB; panfrost uses slot 1 for non-fragment jobs, doc 28 section 4). Cleanup runs in kbase's kill order: hard stop (only if still ACTIVE), AS0 off, IRQ clear.
- **Exact writes**: the scratch window plus the register/value pairs of doc 28 section 7 (AS0 TRANSCFG/TRANSTAB/MEMATTR/COMMAND, JS0 HEAD/AFFINITY/CONFIG/COMMAND_NEXT, JS0 COMMAND = 7 once, JOB/MMU_IRQ_CLEAR = 0x00010001); no GPU_COMMAND, no soft reset, no S2MPU/PMU write. 21 w32 sites are pinned by the source audit (21 mutations); the object audit is the same as `gpupower.o`'s.
- Image size: `--gpu-job` adds 24 KiB of `.bss` (`gpujob_pages`). `build.sh` fails if the default image contains `gpu_job_run`.

## shim --wlan-probe: read-only Wi-Fi path probe (W1; review items D1-D5 fixed, docs/research/20 section 3)

```
cd shim && ./build.sh --board bluejay --avb --pstore-fast --usb-gadget --wlan-probe
tools/probe-run --build-args "--pstore-fast --wlan-probe" --expect "WLAN SUMMARY" --label w1       # read-only, safe to run; never run on hardware yet
```
Source `shim/wlanprobe.c/.h`, host test `shim/wlanprobe_host_test.c` (builds the real file against a fake register window + a faulting address), addresses `soc.json` key `wlan` (generated `WLAN_*` macros). Strictly read-only (object audit: stack-only stores, no `try_write32`).
- **Level 1** (`WLAN pmu+pins` section): `PMU_PCIE1_ISO` / `PMU_PCIE0_ISO` (PMU 0x17463EC4 / EC0, bit 0 = PHY not isolated), `PD_HSI0_CFG/STAT` (0x17462080/84, the HSI0 domain) and `PD_HSI2_A`, `+4` (0x17462180/84, the disabled `pd_hsi2` node, never a gate), the pin banks `GPP14/GPH2/GPA7 CON/DAT` and `WLAN_EN gpp14-2`, `PERST gph2-0`, `CLKREQ gph2-1`, `DEVWAKE gph2-5`, `HOSTWAKE gpa7-0` as `func=X lvl=Y`.
- **Level 2 is gated.** `WLAN CMU_HSI2_PCIE_MUX pa=14400620 v=..` is read only if `PMU_PCIE1_ISO` bit 0 is set; the verdict line is `WLAN l2 gate=open` or `WLAN l2 gated: <pcie1-phy-isolated|pmu-iso-unreadable|cmu-hsi2-pcie-mux-off|cmu-hsi2-unreadable>`, and then **nothing** of the PCIe wrapper is read.
  When open, the groups run in this order with a pstore checkpoint after each: `SYSREG+000..01C`, `ELBI+000..040, 054, 080, 2C8, 2E8, 3A4`, `DBI+000/004/008/00C`, then (last, the riskiest) the PMA lock words `PMA+3F0, E0C, DE8, 5FC, E18` and `WLAN PHY pll_lock= cdr_lock= oc_done= oc_after_rst=`.
- **Always**: `WLAN SUMMARY gate=<open|reason> en=<0|1|?> perst=<..> dbi_id=<8 hex|unreadable> link=<2 hex|?>` (`link` = `ELBI+2C8 & 0x3F`, `11` = L0). All register lines are `WLAN <name> pa=<8 hex> v=<8 hex>` or `... unreadable`.
- On a normal ABL boot the PCIe1 PHY is expected to be isolated (nobody has brought the link up), so the expected first result is `l2 gated: pcie1-phy-isolated` (*guess*); that is itself the baseline for the PCIe bring-up stage.

## shim --wlan-rails / --wlan-en: rail state (read-only) and the first WLAN_EN write (W2; docs/research/29)

```
cd shim && ./build.sh --board bluejay --avb --pstore-fast --usb-gadget --wlan-probe --wlan-rails          # READ-ONLY
cd shim && ./build.sh --board bluejay --avb --pstore-fast --usb-gadget --wlan-probe --wlan-rails --wlan-en  # + the WRITER, only after the rails run and the review
tools/probe-run --build-args "--pstore-fast --wlan-rails" --expect "WLAN RAILS" --label w2r             # never run on hardware yet
```
`--wlan-rails` (`shim/wlanrails.c`, host test `wlanrails_host_test.c`) runs after the level-1 section and before the level-2 gate. Lines (`WLAN ...`):
- `CMU gate ufs_ver=XXXXXXXX open=<0|1>`: the CMU_HSI2 words are read only if the UFS HCI VER (UFS_BASE+0x08, observed 0x300) is readable and nonzero. Then `CMU PCIE_MUX|OSC|QCH_GPIO|QCH_PCIE_A|QCH_PCIE_B|QCH_UFS|QCH_SYSREG pa= v=`
  (offsets +0x620, +0x800, +0x7018, +0x7060, +0x7064, +0x7068, +0x707C of 0x14400000) and `CMU decode ...` lines tagged `(guess)` (bit 4 of the mux = selected, bit 28 of OSC = automatic gating, QCH equality with the UFS word; the QCH bit layout is UNKNOWN).
- `PMU TXCO pa=17463C10 v=` and `PMU decode txco bits 2,3,10 all set=<b>` (read from source: rc.c:80,2698).
- `RAIL L16M|L17M|L18M|L19M(PCIe0|PCIe1 PHY) reg=0x53..0x56 v=0x..` and `RAIL decode ... mode=<0 off|1 standby|2 PWREN_MIF|3 on-normal> enabled= sel= uV= dt_window=` (S2MPG10 over ACPM, `acpm_s10_read`; refuses with zero ACPM traffic like `--gpu-rails`).
- `RAILS l17=<mode>/<uV> l19=<mode>/<uV> cmu_mux=<b|?> txco=<b|?>`: the one summary line.

`--wlan-en` (`shim/wlanen.c`, host test `wlanen_host_test.c`, audits in `tools/test-bluejay-boot.py`): drives WLAN_EN gpp14-2 high for 200 ms and puts it back. Write set (all `w32`, read-modify-write of the masked bits only):

| # | Register | Mask | Value | Cite |
|---|---|---|---|---|
| 1 | GPP14.DAT `0x108401C4` | `0x4` (bit 2) | set | Linux `samsung_gpio_direction_output` = DAT first (`pinctrl-samsung.c:693-696`, `samsung_gpio_set_value` `:555-570`) |
| 2 | GPP14.CON `0x108401C0` | `0xF00` (field of pin 2) | 1 = OUTPUT (observed before: 0 = INPUT, CON `0x33`) | `samsung_gpio_set_direction` `:624-653`; `PIN_CON_FUNC_OUTPUT 1` `pinctrl-samsung.h:61-62` |
| 3 | GPP14.DAT | `0x4` | back to the saved bit (0) | drive low while still an output: `dhd_wlan_power(0)`, `dhd_custom_google.c:616` |
| 4 | GPP14.CON | `0xF00` | back to the saved field (0 = input) | `samsung_gpio_direction_input` `:656-676` |

Preconditions (otherwise ZERO writes, `enable=refused`): counter running; GPP14.CON/DAT and GPH2.CON/DAT readable; WLAN_EN CON field exactly 0 and DAT bit 2 = 0; PERST (gph2-0) DAT bit 0 = 0 and its CON field <= 1.
Lines: `WLAN EN before GPP14|GPH2 ...`, `EN after enable CON= DAT=`, (after 200 ms on the counter) `EN sample GPP14|GPH2 ...`, `EN restore CON= DAT=`, and the summary
`WLAN EN enable=<ok|refused|fault> sample=<en perst clkreq devwake hostwake: 0/1/?> restore=<ok|FAILED|n/a>`. Any write that faults, or a read-back that differs, is `fault` and skips the wait; the restore runs in every case that attempted a write.
A failed restore prints `!!! RESTORE FAILED ...` and `restore=FAILED`; (guess) the pin state is volatile and does not survive a reset. Restore order (DAT low, then input) deliberately differs from "reverse of the enable" so the module is driven off before the pin is released.
The writer touches no PERST, no PHY isolation, no SMC, no PMIC rail. Expected quiet result: sample `10100` (*guess*: nothing in the chain reacts without the PHY/PERST); that is not a failure (research 29 U3/U8).
Also printed read-only before and after: `EN before|after pads GPP14 PUD= DRV=` / `CONPDN= PUDPDN=` (GPP14 +0x08/+0x0C/+0x10/+0x14, `pinctrl-exynos-arm64.c:47-50`): what the pin floats to after the restore and the drive of the high level.
**Polarity: do not "fix" it.** The DT flag `0x1` of `wl_reg_on` / `pcie,wlan-gpio` is GPIO_ACTIVE_LOW, but rc.c (`:1989-1995`) and dhd (`dhd_custom_google.c:529-566,601`) use the legacy raw GPIO calls (`gpio_direction_output(.., 1)` = ON); *guess* (Linux gpiolib legacy API, not fetched): they ignore the flag, so raw HIGH = module on.

## shim --wlan-smc-read: two EL3 SMC READs of the PHY-isolation word (W2a; docs/research/29 section 12)

```
cd shim && ./build.sh --board bluejay --avb --pstore-fast --usb-gadget --wlan-probe --wlan-smc-read
tools/probe-run --build-args "--pstore-fast --wlan-smc-read" --expect "WLAN SMC summary" --label w2a      # never run on hardware yet
```
`shim/wlansmc.c` (host test `wlansmc_host_test.c` with a fake EL3). The only SMC of the object `wlansmc.o` (the rest of the image keeps shim.c's PSCI calls and `prepare_bluejay_cpu`'s option-1 WRITE of CPU_INFORM; review 2 F5): `smc #0`, **x0 = 0x82000504** (SMC_CMD_PRIV_REG, `exynos-smc.h:45`), **x1 = 0x17463EC4** then **0x17463EC0**,
**x2 = 0 (PRIV_REG_OPTION_READ**, `exynos-el3_mon.h:21`), **x3 = 0**; results a0..a3 printed. Lines (`WLAN ...`): `SMC read pa=17463EC4 ret=<int32 a0> a0=..`, `SMC read a1= a2= a3=`, `SMC cmp pa= mmio= same=<yes|no|error>`,
`SMC mmio after pa= v=` (`!!!` if a READ changed it), and `SMC summary same=<yes|no|error> ambiguous=<0|1>`. `error` = a0 negative as int32 (EL3 refusal -EINVAL = -22, mainline `exynos-pmu.c:65,82`; SMCCC unknown function -1,
`linux-arm-smccc.h:247-249`) or the MMIO read faulted. `ambiguous=1`: a0 == 0 and the MMIO word == 0 (observed 0): the reply cannot tell a value from a status 0 (*guess* that the first run is ambiguous). Both reads, no write, no loop; the
helper refuses (no SMC) any other address. Audits: exactly one `smc` instruction in the object, x0/x2/x3 constants and the address guard checked in the disassembly (`check_wlansmc_object_text`, 9 object + 13 source mutations).

## shim --wlan-iso: release the PCIe1 PHY isolation through EL3 and put it back (W2c; docs/research/29 section 12)

```
cd shim && ./build.sh --board bluejay --avb --pstore-fast --usb-gadget --wlan-probe --wlan-iso      # implies --wlan-smc-read; WRITER via EL3, only after the Opus review
tools/probe-run --build-args "--pstore-fast --wlan-iso" --expect "WLAN ISO release=" --label w2c       # never run on hardware yet
```
Code: the `WLAN_ISO_RUN` block of `shim/wlansmc.c` (object `wlansmc_iso.o`), host test `wlaniso_host_test.c` (fake EL3 with RMW / full-overwrite / ignore / refuse / lie / late semantics). **SMCs of the object `wlansmc_iso.o`** (scope: that object only; shim.c's PSCI calls, the reboot-to-bootloader SYSIP_DAT0 WRITE and the CPU_INFORM clear remain in the image):
(1) `smc #0` x0 = `0x82000504`, x1 = `0x17463EC4` or `0x17463EC0`, x2 = 0 (READ), x3 = 0 (the W2a reads, plus one READ of `0x17463EC4` as a precondition); (2) ONE RMW set: x0 = `0x82000504`, x1 = `0x17463EC4`, x2 = 2, x3 = 1 (mask), x4 = 1 (value);
(3) exactly ONE RMW clear after any sent set: the same with x4 = 0 (x5..x7 = 0 in both RMW calls, as Linux). Nothing else in `wlansmc_iso.o` (no WRITE option 1, no other register, no other mask or value). Linux: `exynos_pcie_phy_isolation()` = `rmw_priv_reg(pmu_alive_pa + 0x3EC4, 1, val)` (`pcie-exynos-rc.c:393-407`), as the FIRST step of
`exynos_pcie_rc_poweron()` (`:3387-3389`, before clocks/pinctrl/PHY init, no delay) and the last of `rc_poweroff` (`:3667-3669`); `PCIE_PHY_BYPASS = 1` = isolation released (`common.h:51-52`).
Preconditions (else nothing is sent, `release=refused`): timer running; `0x17463EC4` bit 0 == 0; `0x17463EC0`, GPH2.DAT, GPP14.DAT readable; PERST gph2-0 low; WLAN_EN gpp14-2 low; the SMC READ of `0x17463EC4` returns a0 >= 0 AND a0 == the MMIO word (F3). **A `--wlan-iso` image never reads the PCIe wrapper (level 2 and its SUMMARY are compiled out) and cannot contain `--wlan-en` (`build.sh` refuses the combination and `shim.c` has an `#error`; F2).**
After the set: bounded poll (<= 10 reads, 100 us apart; OUR bound, Linux has none): `ok` needs bit 0 set, a0 == 0, every other bit of `0x17463EC4` unchanged and `0x17463EC0` unchanged. a0 != 0 with nothing changed = `refused`; the word unreadable = `fault`;
anything else (accepted but unchanged, a0 != 0 but changed, other bits moved: e.g. the EL3 overwrites instead of read-modify-write) = `mismatch`. **The one clear RMW is sent after every sent set (F1; idempotent if the bit is 0)**, never a second set; it must read back bit 0 == 0 in the bounded poll AND in one more read 10 ms later (`ISO final ISO1=`).
Lines: `ISO before ISO1= ISO0=`, `ISO rmw set ret= a1=`, `ISO after set ISO1= ISO0=`, `ISO rmw clear ...`, `ISO after clear ...` and `WLAN ISO release=<ok|refused|fault|mismatch> readback=0x.. restore=<ok|FAILED|n/a>`; `!!! RESTORE FAILED` if the bit stays set.
Known blind spots (*guess*): the observed word is 0, so an overwrite instead of RMW is indistinguishable from the guessed semantics at this value; an EL3 whose late apply survives the clear and lands after the 10 ms final look would still leave the bit set (the final line would not show it); *guess*: bit 0 = 1 means the isolation cells are bypassed, not that the PHY is powered (its supplies are the LDO rails).

## Runtime MMIO pokes: `sweettouchpoke=` (xnu patch 0093)

Test a register hypothesis without a CI kernel rebuild: `tools/xrun ... --args "sweettouchclk=3 sweettouchpoke=r:10c00000;m:1e080000=1=1;d:1000;r:1e080000;"`.
Script = hex ops, **every op ends with `;`** (a script cut by the boot-args limit is a syntax error), no spaces: `r:ADDR;`, `w:ADDR=VAL;`,
`m:ADDR=MASK=VAL;` (read-modify-write), `d:US;` (0..1000000). Runs in the touch thread after `tg_cmu_enable()` and right before the SPI setup
(skipped with one line when the mux check leaves the bus alone); each op prints `SWEET TOUCH: poke N ...`. `w` does not read first; inside
SPI11 neither `w` nor `m` reads back (RX_DATA pops on read).
ADDR: physical, 4-aligned, inside SPI11 (0x1000), the USI11 sysreg page (0x1000 size is a guess), CMU_PERIC1, CMU_TOP or a touch pin bank.
CMU_PERIC1 and CMU_TOP are read-only except the words in the ADT: PERIC1 usi mux, bus mux, both auto regs, ipclk/pclk/rstnsync gates, QCH
and DIV (OPTION never), TOP mux/div/gate. Max 32 ops, 240 characters; anything bad prints one `poke: rejected (...)` line and runs nothing.
The whole boot-args line is 607 characters; `tools/mkpayload.py` now exits with an error beyond that. Parser and range table:
`bsd/dev/sweet_touch_poke.h`, tested by `shim/touchpoke_host_test.c`. Run on hardware (2026-10-22, runs touchx3 and later: `poke: 3 ops` lines, reads back the written values); a string without the final `;` is rejected (AP-10).
Order is MASK before VAL: "bit4 := 0" is `m:10c00640=10=0;`, the oscillator recipe of patch 0095 is `m:10c00640=10=0;d:100;m:10c0180c=f=5;`. The string `m:10c00640=0=10;d:100;m:10c0180c=5=f;` in the 0095 header comment is wrong (MASK 0 = no-op; *observed* in runs oriole-gui3/gui4: `mask 0: 0x10 -> wrote 0x10`); `tools/run-preflight` now fails any `m:` op with MASK 0.

## FTM5 post-reset steps: `sweettouchsysinfo=`, `sweettouchspi4=`, `sweettouchidwait=`, `sweettouchidretry=` (xnu patch 0145)

All default 0 (the patch 0095 path, unchanged). The first two need `sweettouchcs=1` (they run after its controller-ready event, else one
`skipped` line); order: sysinfo, spi4, idwait, identify, retries. Sources (*read from source*, fts_touch f87fd67 `ftm5/`): Linux probe =
`fts_system_reset` (reset gpio low, 10 ms, high, poll the FIFO for `03 00`, ftsCore.c:66-108) then `readSysInfo(info, 0)` (fts.c:5343-5352),
a framebuffer read `A7 00 00` + 1 dummy (ftsCore.c:557-559); Linux never reads DCHIP_ID 0x20000000 at probe. The SPI4 write (ICR 0x2000002D
:= 0x02) exists only in `hold_m3` of the flash path (ftsFlash.c:283-343, "when no fw is running the chip use SPI3 by default"), after
`fts_system_reset`, 100 ms and the M3 hold (ftsFlash.c:1046-1062).
- `sweettouchsysinfo=1`: read 18 bytes of System Info, lines `sysinfo raw: ..` and `sysinfo: signature a5? type 01? chip0Id .. fwVer ..`. Read only.
- `sweettouchspi4=1` ICR only; `=2` GPIO dir 0x10, pull-up 0x02, config3 0x70 (ALIX 36 48), config0 0x30, ICR 0x02, 1 ms; `=3` 100 ms + hold M3
  (`FA 20 00 00 24 01`) + the block of 2 (firmware stopped; phase 2 pulses the reset again). Lines `spi4: <step> [addr] := val (6 bytes): sent`
  and `spi4: ICR [0x2000002d] before|after = 0x..`.
- `sweettouchidwait=0..1000` ms before each identify; `sweettouchidretry=0..5` extra identifies while it does not match (`idretry k/N`).
  Out-of-range values: one `refused ...` line, all four off.
Oriole recipe, one variable per run vs oriole-gui5 (CR-08), each with `--rails`:
A (read-only additions) `sweettouchmux=1 sweettouchcs=1 sweettouchsysinfo=1 sweettouchidwait=100 sweettouchidretry=3 sweettouchpoke=m:10c00640=10=0;d:100;m:10c0180c=f=5;`;
B = A + `sweettouchspi4=2` (changed vs A: SPI4 block); C = A + `sweettouchspi4=3` (changed vs B: M3 hold). Status: not run (no device result yet).

## Fuel-gauge reader: `sweetbatt=1` (xnu patch 0148, plan step C4; reviewed SAFE docs/reviews/0148-hsi2c-fg-review-fable-1.md; RUN ONCE: o2-c4-batt1 FAIL fail-safe)

Default off: without `sweetbatt=1` (absent or `0`) the patch maps nothing and reads nothing (*read from source*: `sweet_batt_init()` returns
before any ADT lookup; pinned by `tools/test_hsi2c_fg_audit.py`); any other value prints `SWEET BATTERY: refused sweetbatt=N ...`. It also needs
the ADT node `/sweet-batt` (`tools/mkadt.py`, only boards whose board.json has an `fg` block, today oriole). Not in `xnu_bootargs_default`.
What it does: the controller sequence of the reviewed shim `--fg-read` (branch wip/shim-fg-read, `shim/fgread.c`, review
`docs/reviews/fgread-review-fable-1.md` there: SAFE after F1-F4) from an XNU kernel thread, 20 s after `sweet_ufs_init`, then every 30 s:
I2C READs of the MAX77759 fuel gauge (7-bit 0x36 only) on HSI2C12, registers 0x06 0x08 0x09 0x0A 0x0B 0x35 0x21 only, each = one pointer
byte written + a repeated-start 2-byte read. Same seven writable controller registers, values, order, preconditions (i2c12 gate, pads F4,
TIMEOUT/TRAILING already Linux's), bounded waits (10 ms of counter and 100000 polls per message) and F1-F3 restore as the shim; additionally
the live controller must equal the handoff words observed in runs oriole-peri1/oriole-fg1 (board.json `fg.handoff`). Each register transfer
and the restore run with preemption disabled (at most ~30 ms on a fault path). No reset, no reconfiguration, no bus recovery, ever.
Lines: first set `SWEET BATTERY: before ...` (4), `restore wait idle=1`, `restore ...` (3), `raw 06=XXXX 08=XXXX 09=XXXX 0A=XXXX 0B=XXXX 35=XXXX
21=XXXX restore=ok|FAILED`; every set `SWEET BATTERY: soc=NN.NN% v=NNNN mV i=+-N.N mA t=NN.N C src=fg` (i = AvgCurrent with the register sign,
negative = discharging; Linux reports the opposite sign). Any refusal or failure: `SWEET BATTERY: refused <reason>` (a failed register:
`refused <NAME> reg=0xNN nack|timeout|error restore=ok|FAILED`), `!!! RESTORE FAILED` lines if the read-back differs, then
`SWEET BATTERY: reader stopped, no further HSI2C12 access this boot`. Code: `bsd/dev/sweet_hsi2c_fg.h` (pure, all access through
`struct sfg_ops`, one writer `sfg_w32()`), `bsd/dev/sweet_hsi2c.c` (glue). Tests: `shim/hsi2c_fg_xnu_host_test.c` (also ASan/UBSan, built by
`shim/build.sh`), `tools/test_mkadt_batt.py`, `tools/test_hsi2c_fg_audit.py`.
Result 2026-11-15 (observed, o2-c4-batt1, CI kernel 14c9780): handoff words as the shim saw, first RepSOC read errored, one RX FIFO byte left -> `restore=FAILED`, reader stopped. Rule (0148 F7): after `restore=FAILED` or `BUS NOT IDLE`, power-cycle the phone before the next experiment. Run form: `tools/xrun ... --args "sweetbatt=1"`, expect `SWEET BATTERY: soc=`; never with a shim `--fg-read` image
in the same boot (the bus could be left mid-transfer, review F5).

| Step | Status | Tag |
|---|---|---|
| C4 hsi2c-fg (XNU fuel-gauge reader) | built, unrun (awaiting review): patch 0148 applies on the stack (tools/stack-apply PASS), local kernel build RESULT PASS sha256 88444aa3 (~/pixelios-artifacts/xnu-local-c4-9225e36), host test PASS normal + ASan/UBSan, audit PASS; on hardware: nothing yet | observed (host builds/tests only, 2026-11-14); hardware behaviour: guess until a run |

## tools/probe-run: one shim probe cycle, one report

```
tools/probe-run [--touch-probe[=OPTS] | --build-args "..."] [--expect REGEX]... [--timeout S] [--label NAME]   (also: tools/pix probe)
tools/probe-run --dry-run --from-dir DIR [--expect REGEX]...   # re-read DIR/usb.txt, writes nothing
tools/probe-run --self-test      # stub phone-state/to-fastboot/fboot/build/usb-log; run by check-all.sh
```
Steps, all bounded: `phone-state --expect fastboot --wait 20` (else `to-fastboot.sh` once, else ERROR with the instruction) -> `cd shim && ./build.sh
--board bluejay --avb --usb-gadget ARGS` (`--touch-probe` adds `--pstore-fast`, as the section below) -> `usb-log.py capture` in the background
**before** -> `fboot.sh --expect probe shim/out/shim-boot-avb.img` -> poll the log until every `--expect` regex matches or `--timeout` (60 s; default expect
for `--touch-probe`: `SWEET TOUCH RESULT`) -> wait up to 30 s for the phone back in fastboot or recovery (recovery -> `to-fastboot.sh`). The phone
legitimately leaves USB while the shim reboots, so absence right after the result line is normal: PASS with a "not back" note.
Run dir `logs/probe/<UTC>-<label>/`: `usb.txt`, `build.log`, `fboot.log`, `report.txt`. Report: `RESULT: PASS|FAIL|HANG|ERROR ...`, up to 12 key lines
(touch probe: the `SWEET TOUCH RESULT` lines, then `VARIANT`/`identify`/`loopback`/`rst`/`ACPM` lines), `dir: ...`. Exit 0 PASS / 1 FAIL or HANG / 2 ERROR.
FAIL = line missing, phone back. HANG = line missing and phone not back in 30 s (hold Power+VolDown). ERROR = not in fastboot at start, build or fboot failed.
Only bluejay (fboot.sh/phone-state refuse others); never raw `fastboot boot`. Verified with stubs and a dry-run of a real capture only, not on hardware.

## shim --touch-probe: touch SPI11/USI11 bring-up in seconds, no iOS boot

```
cd shim && ./build.sh --board bluejay --avb --pstore-fast --usb-gadget --touch-probe[=run[,mux][,cs][,osc|,top][,loop][,rst][,misopu][,railsr|,rails]]
tools/fboot.sh --expect probe shim/out/shim-boot-avb.img     # then read the RAM/USB/pstore log as for the other probes
```
Source `shim/touchprobe.c/.h`, host test `shim/touch_host_test.c` (run by `build.sh`), constants from `soc.json`/`board.json`
through the generated `TOUCH_*` macros (`tools/boards.py`), the same keys `tools/mkadt.py` puts into `/sweet-touch`. Options are
build-time (the shim builds in seconds); an unknown word, `osc` with `top`, or options without `run` make `build.sh` exit 2.
- `--touch-probe`: phase 1, **reads only**: CMU_PERIC1 + CMU_TOP regs, the USI11 sysreg mode, the seven pads. The USI/SPI registers
  are read **only if the sysreg mode already is SPI** (at handoff it is 0: `diag before: skipped: usi mode=0`; a hang cannot be
  caught by `try_read32`). A bus-faulting register prints `unreadable pa=...` and the dump goes on.
- `--touch-probe=run`: phase 2 after a pstore checkpoint, the writes of XNU `sweettouchclk=3 sweettouchvar=1` (patches 0088-0092:
  CMU_TOP gate, USI mux + three gates + QCH, USI mode, SPI pads, engine init, chip-id read, variants A-E until one burst completes).
  SPI/USI registers are read only after the mode was written. At handoff the mux pad gpp26-1 is an **input** (`func=0 dat=0`), so
  plain `run` writes nothing and says `leaving the bus alone`; add `mux` for that.
- Options (docs/research/21 section 3), order inside a run: dump -> `mux` -> clock chain -> `osc`/`top` -> SPI setup -> `cs` (variant F)
  -> `loop` -> `rst` -> identify (+ `misopu`) -> variants A-E (only without experiment options) -> restore:

  | Option | What it writes | Notes |
  |---|---|---|
  | `mux` | gpp26 pin 1 only: DAT := 0 first, then CON := output, as XNU 0088 `sweettouchmux=1` | read-back checked; a mismatch stops the run |
  | `cs` | X1: SPI setup is the ABL `spi_init` (USI_CON/OPTION, FB_CLK; no MODE_CFG/INT_EN/SWAP writes); variant F = GPIO CS low, `CS_REG &= ~1`, `PACKET_CNT = n\|0x10000`, TX fill, `CH_CFG \|= 3`, poll RX >= n, `CH_CFG &= ~3`, `PACKET_CNT = 0`, SW_RST, GPIO high, `CS_REG \|= 1`; identify then uses the same style | |
  | `osc` | X2: USI mux `0x10C00640` bit4 := 0, poll BUSY (bit16), divider `0x10C0180C[3:0]` := 5, poll BUSY | restored if no burst moved data |
  | `top` | X6: bit4 := 1, divider := 9 (40 MHz ipclk) | divider is writable only with `osc`/`top` |
  | `loop` | X3: `MODE_CFG` bit3 (self-loopback) with `FB_CLK` 0 then 1, 5-byte TX/RX compare | restores MODE_CFG / FB_CLK |
  | `rst` | X4: PUD := 0 on clk/mosi/miso/cs/reset, reset pad low 10 ms then high, polls the FIFO (cmd 0x87, same shape as XNU) up to 1.5 s for event 0x03 | the pulls are restored at the end; first 8 non-zero events logged |
  | `misopu` | X5: MISO PUD := 3 during the identify | restored |

  Any experiment option drops the QCH write (research 21: it hands the clock to the Q-channel handshake) and the A-E ladder.
- `railsr` / `rails` (docs/research/23, `shim/acpm.c`; need no `run`): the ACPM client. Section `TOUCH rails (ACPM)`, in three steps with a pstore
  checkpoint after each, before `run`: (0) describe the IPC channel (reads only: INIT, channel table, the descriptor of channel 2, queue pointers, INTSR0;
  `SWEET TOUCH: ACPM ...` lines), (1) READ the two enable registers (`railsr`), (2) only with `rails`: UPDATE bit 7 (value = mask = 0x80, never clear) of 0x5C then
  0x5D, ~10 ms, read both back. Every transaction prints `seq`, `req=` / `rep=` words, `ret`, `val`, `err`, `writes`, `published`, `ticks` and `us`. Replies are
  bounded to 50 ms, no retries. Nothing is ever rolled back once the request queue's front is written (the APM may be running the request): a timeout / fault then
  prints `STATE seq=.. req rear=.. front=.. rsp rear=.. front=..` and leaves the channel as it is (the next run refuses BUSY until a human has looked).
  **Gates (soc.json `acpm`)**: while `layout_verified` / `elem_unit_verified` are 0 (the shipped state) or a rail `vsel_min/vsel_max` is null, the shim prints
  `ACPM unsure: <what>` and writes NOTHING. The describe step prints the descriptor words, the pointers, the first 48 raw words of both queue buffers (one 16-byte slot per
  `ACPM req|rsp buf+OFF` line) and, on a refusal, the exact reason, WHATEVER the validation said. `vsel_min/max` ship set (L25M 0x24-0x32, L26M 0x1C-0x3C, derived from the regulator
  source and the DT limits, checked by `tools/boards.py`). Staging: `railsr` (gates closed) -> compare the dump (doc 23 erratum, `docs/HANDOFF.md` SESSION 12) -> set the two flags ->
  `railsr` again (a queue-slot write + doorbell per register, the APM reads the register: no PMIC change) -> `rails`. Both flag-set steps WRITE to the APM's SRAM: they need the Opus re-review of the fixes first.
  Last lines: `SWEET TOUCH RESULT: rails read: L25M=off L26M=off ok` / `rails update: L25M 0x23->0xa3 L26M ... on=2/2 ok`.
  Status: *observed on hardware* (run rails2, 2026-10-22: UPDATE accepted, readback `on=2/2`; the rails are off again after a reset, so the shim switches them on before the XNU jump with `--rails`); also host tests (`acpm_host_test.c`, `rails_host_test.c`).
- Safety: every write goes through one helper `w32`, which asks `w32_allowed` (per register, the exact bits that may change; a write
  that changes another bit is refused, so a bit-wise RMW bug cannot spill). Not writable: CMU OPTION, any other divider, CMU_TOP mux/div,
  the irq pad, the mux pad's pull, SPI STATUS/RX_DATA, USI wrapper words other than CON/OPTION. `tools/test-bluejay-boot.py` audits the
  source (no asm, no volatile, no pointer casts, `if (!w32_allowed(...)) return` before the write, allowlist depends on the address) and
  the object (stores only stack/argument based, `try_write32` only in `w32`), with mutation self-tests.
  All polls have a time limit and a poll budget; the fixed delays (a few us, the 10 ms reset pulse, 5 ms between ready polls) use
  `wait_ticks()` and so are not protected against a dead counter.
- Output: the XNU driver's own lines (`SWEET TOUCH: cmu base ...`, `pin clk ...`, `VARIANT F: ...`, `identify: ...`), label `SWEET` + text
  `TOUCH: ...`, so `grep 'SWEET TOUCH:'` of the RAM/USB log feeds the same parser as the XNU log; lines over the 96-byte RAM-log line are
  split at spaces with key=value tokens whole. The UART copy reads `SWEET: TOUCH: ...`. The last line is
  `SWEET TOUCH RESULT: opts=0x.. mux=.. clk=.. F=.. loop=.. rst=.. id=.. moved=F,L,R,I` (F variant F, L loopback, R reset FIFO read,
  I identify; `moved=-` means nothing moved; `id=chip-id-ok` is the FTM5 id).
- Status: *observed on hardware* (run touch1, 2026-10-22: identify `36 48 03 00` with rails on); the recipe is in XNU patch 0095 and boot-arg `sweettouchcs=1`.

## tools/phone-lock: one lock for the one phone (TP-016)

```
tools/phone-lock acquire --owner NAME [--wait S] [--ttl S] [--pid N] | release --owner NAME | status | force-release --yes
tools/phone-lock run --owner NAME [--wait S] [--ttl S] -- CMD...     # acquire, run CMD, release in a finally (also SIGTERM/INT/HUP)
tools/test_phone_lock.py                                              # host test, run by check-all.sh (unittest discover)
```
**Enforcement and queue (TP-020, 2026-10-24; review 11 fixes).** Take the phone only when you are ready to use it: `xrun` and `probe-run` build the image first and take the lock
afterwards (`--lock-wait S` waits in line; default fails at once with who holds it) and keep it until they exit. Identity is a **token**, not the owner name (the name is only a label):
`phone-lock run` and the self-locking tools create a token and export `PHONE_LOCK_TOKEN` to everything they start. `phone-lock check [--token T]` exits 0 only if that token is
the live lock's; `fboot.sh`, `fflash-userdata.sh`, `run-probe.sh`, `pstore-pull.py run` and `phone goto` (not `--dry-run`, not `state`) call it and refuse without it. So a shell that
did not get the lock from `run`/`xrun`/`probe-run` cannot flash or boot: wrap manual slots as `tools/phone-lock run --owner NAME --wait S -- bash -c '...'`. A lock with a pid ends only
when that pid is dead and its process group (`run` starts CMD in its own group) is empty, never by ttl; a bare `acquire` without `--pid` is ttl-only and warns. Waiters (`--wait`) are served in
arrival order (`$PHONE_LOCK_DIR.queue`, shown by `status`). `tools/phone-guard` is a Claude Code PreToolUse hook (wired in the super repo `.claude/settings.json`) that denies hand-typed
mutating `fastboot`/`adb` (also inside `bash -c`/`eval`); read-only queries and `adb shell getprop` pass; it is a guard rail against accidents, not a sandbox; sessions pick it up at start. Gates and builds never take the lock.
Not guarded: `backup-partitions.sh`, `check-udump.sh` (adb reads, run by the user with root). Tests: `tools/test_phone_lock.py`, `tools/test_phone_guard.py`, `tools/test_phone_sm.py`, `tools/probe-run --self-test`.

**Fewer false alarms, no cut tails (TP-020 follow-up, 2026-10-24).** `probe-run` waits up to 120 s (`PR_BACK`) for the phone after a shim run (it leaves USB for about 30 s and returns in recovery: observed by the Wi-Fi agent, HANDOFF session 28, not re-measured) with the
capture still running; a slow return is a PASS note, a run that hit `--timeout` while the shim was still logging (log younger than 15 s) is a FAIL "raise --timeout", and HANG / "hold Power+VolDown" needs the
expected lines missing AND no Google USB device on the bus after the window (on the bus but not fastboot/recovery: FAIL pointing at `phone-state`). `xrun` waits up to 120 s (`XRUN_USB_WAIT`) for the phone to
appear, after it holds the phone lock, before asking for a button hold; a hard timeout shorter than `--restart` is PARTIAL (reset not awaited); when the reset was due but the phone is not back at the hard
timeout there is a 90 s grace window (`XRUN_GRACE`, so the cap is `--hard-timeout` + 90 s), and if the bluejay itself is then in recovery/fastboot the run is scored PASS as "reset inferred" (not observed).
`usb_gone` in run.json is when the phone left fastboot for XNU (+2 s), not proof that the reset fired. Log tails (unit-tested on a pty / loopback socket, not yet seen on hardware): `usb-log.py capture` drains the
tty (bounded to 2 s) after the device node vanishes and keeps a last partial line on SIGTERM; `usbnet-log.py` drains queued datagrams (bounded to 2 s) after the stop file appears. Two 2026-10-22 runs (`dvfs4-all`, `busy2`) were scored FAIL although the reset fired: `report.revised.txt` next to their `report.txt` (wording: reset inferred, not observed).

**One set of phone-path tools for every agent (TP-021).** The agent-independent tools listed in `tools/common-files.txt` (lock, phone, fboot, fflash, xrun, probe-run, log capture, guard hook and their tests) have one
source of truth: the branch `common-tools` (own worktree `~/.t3/worktrees/common-tools/kernel`; change them there). In any other worktree, at a task boundary and with the phone lock free:
`tools/tools-sync status` (per file same/differs/missing) and `tools/tools-sync pull` (copies differing files from the ref, skips files with uncommitted changes, refuses while the lock is held; you review and commit). An agent-specific flag in a common file
must be upstreamed to `common-tools`, or it shows as drift. The gate does not check this (drift is a warning for the agents, not a gate failure).

**Runs that end when the answer is in (boot-speed B1/B2, 2026-10-24).** *Observed on hardware* (run `bl1-remote3`, kernel with patch 0100): `xrun --early --stop-on RX` stops at the pattern and `phone goto fastboot` resets the phone over the USB link (UDP `SWEET-RESET-BL`, boot-arg `sweetremote=1`);
the phone was in fastboot 95 s later instead of the ~700 s a `--restart 400` run waits. So `--early`/`--stop-on` now switch `--remote-reset` on by default (`--no-remote-reset` to disable), `phone goto` waits 150 s (was 90) before it asks for a button hold,
and `--fail-on RX` (default with an early stop: `Kernel trap at|panic\(cpu|panic:|Assertion failed`, `--fail-on none` to disable) ends a failing run at once and scores it FAIL, so a failing run no longer waits out the restart window. Observed NOT working (run `bl2-timer`, 2026-10-24): `--restart N,bootloader` alone (timer path) came back at restart+152 s as 18d1:d001, the same as a plain timer reset, not as fastboot (18d1:4ee0); use the remote reset.

**Kernel log timestamps (boot-speed M1, patch 0115 on branch boot-speed-patches; review 17).** With boot-arg `sweetlogts=1` every console line in the persistent/USB log starts with `[SSSSS.mmm] ` (raw timer seconds: includes ABL and shim time, subtract the first stamp for time since kernel entry). The stamp is when the line's first byte reaches the console drain, not the printf time (output printed with preemption disabled is drained later): read a gap as "between drains" and confirm it with a second marker. `tools/xrun_report.py` strips the prefix before matching (merged.log stays raw) and prints `[kernel stamp N.NNN]` on the marker lines. With `sweetfb_noconsole=2` (patch 0116) keep one run without it: a hang that is not a panic shows nothing on screen while the console is muted.

**shim/out and `tools-sync` safety (review 16).** `xrun` and `probe-run` take `shim/out/.build.lock` around anything that writes `shim/out` (the gate no longer does: it builds into `shim/out/gate/` and holds `shim/out/.gate.lock`); `xrun`/`probe-run` boot a private copy of the image from the run directory and delete it after fboot. Under `phone-lock run` they wait only 60 s for a gate (the phone is held), otherwise up to an hour. `tools/tools-sync pull` refuses while the phone lock is held or unreadable, while that build lock or the gate lock is held, and for a file this branch changed in its own commits (shown with the log; `--force` overrides); it checks out all files in one command. Never edit `tools/xrun`, `probe-run` or `check-all.sh` in a worktree that has a run waiting or running from it (bash reads a script while it runs, AP-18): run hardware from a worktree you do not edit.

**One numbering for `xnu/patches` (TP-022).** Patches stay per branch (a kernel build contains only the patches of the branch that builds it; sharing one stack was tried and rejected by the user on 2026-10-24: every agent's
experimental patch would enter everyone's kernel and make failures harder to attribute). Branches diverge (GPU holds 0102-0111, everyone else stops at 0101), so a second agent writing "the next patch" would pick a number that is already taken.
Before writing a patch run `tools/patch-numbers next --owner NAME --name SLUG`: it prints the next free number above every local and remote branch and every reservation, and records the reservation as a git ref
(`refs/patch-numbers/NNNN`, shared by all worktrees). `tools/patch-numbers status` lists every number, who holds it and CONFLICT lines (same number, different file name on two branches); `tools/patch-numbers check` fails if THIS
tree uses a number another branch has under a different name; `release NNNN` drops an unused reservation. When the branches are merged, the numbers are already in order. The gate does not run it (another branch's state must not fail
your gate): run `check` before committing a patch and before a merge. Enforcement (two hooks): `tools/patch-guard` is a Claude Code PreToolUse hook (super repo `.claude/settings.json`) that denies creating a NEW `xnu/patches/NNNN-slug.patch` (Write/Edit tool, or a Bash redirect/cp/mv/tee/git mv) unless `refs/patch-numbers/NNNN` reserves that number for that slug
(`tools/patch-numbers next ...`; a patch you already wrote: `tools/patch-numbers claim NNNN --owner NAME --name SLUG`); editing an existing patch is always allowed. `.git/hooks/pre-commit` (installed with `tools/patch-numbers install-hook`, shared by every worktree,
runs the script from the common-tools worktree) refuses a staged NEW patch whose number another branch or reservation holds under a different name, and reserves the number of a clean one so nobody else picks it. Guard rails, not sandboxes, and both fail OPEN: the settings.json commands are wrapped so a missing or broken script exits 1 (non-blocking) and never 2 (which blocks every tool call), the git hook only blocks on a real overlap (exit 3 of `precommit`) and lets the commit through, with a message, on any internal error. The guard looks at the repo the PATH belongs to (last `cd` in the command), not at the session's cwd. `tools/tools-sync` can also sync a directory (manifest line ending in "/"), but no patch directory is in the manifest.

Lock = directory `$PHONE_LOCK_DIR` (default `~/pixelios-artifacts/phone.lock`, outside every worktree), created with `os.mkdir`, holding `info.json`
(owner, pid, start, ttl, cmd). Stale (holder pid dead, or ttl over; default ttl 1800) = taken over and the takeover is printed. Same owner re-acquiring refreshes.
`release` by another owner exits 2; `force-release --yes` removes any lock. `run` records its own pid; a bare `acquire` records pid 0 (only the ttl ends it)
unless `--pid N`. Owner default: `$PHONE_OWNER`, else `agent-<pid>`. Last line `RESULT: PASS|FAIL|ERROR owner=NAME held_for=Ns`; exit 0 ok, 1 busy after `--wait`
(max 3600; holder and age printed) or, for `run`, CMD's own exit code, 2 error; `status` exits 0 free / 1 held. Touches no device. Tested on the host only.

- **`tools/lint-c` and the gate's build dirs (round 3 F6).** The gate builds into `shim/out/gate/build-*` and no longer refreshes `shim/out/board_config.h`;
  `tools/lint-c` reads `shim/out` by default (RESULT: ERROR if the header is missing, but an old one is accepted), so after a gate run use
  `tools/lint-c --out shim/out/gate/build-0`.
- **Memo limits (round 3 F4/F5).** The memo compares a dependency's hash on every open (an edit + revert during a run stores nothing), records
  `os.listdir`/`os.scandir`/`glob.glob` results as inputs (hash of the sorted names, re-listed on a hit; by fd or with `root_dir` = not stored),
  and does NOT track failed opens of missing files or `os.stat`/`Path.exists` checks (a memoized function must not branch on those).

## tools/ref-capture: stock Linux reference from recovery (TP-034)

```
tools/ref-capture [--out DIR] [--only NAME,...]     # phone in recovery, ro.product.device == $BOARD; run under tools/phone-lock run
tools/ref-capture --list                             # the allowlist; touches nothing
```
Read-only capture of the stock kernel's view of the phone (dmesg, /proc/interrupts, /proc/iomem, a tar of the device tree, pinctrl pins and pinconf,
regulators name/state/microvolts, power_supply uevents, thermal zones, /proc/modules, bus device lists, debugfs gpio, regmap names, the first 2 KiB of `misc`
(BCB; strings extracted on the host to `misc-bcb.txt`). Each command is a fixed allowlist entry wrapped in `timeout 8` on the phone and bounded on the host;
`refusal()` rejects `clk_summary` (crashed the phone once, observed by the user), regmap `registers`, redirections and write verbs before anything is sent.
One line per item (`OK name bytes` / `EMPTY` / `TIMEOUT` / `REFUSED`), last line `RESULT: PASS files=N dir=DIR` (exit 0) / FAIL (1: a timeout or nothing
captured) / ERROR (2: not recovery, wrong device). Output under `~/pixelios-artifacts/ref/<board>/<UTC>/` (never in git). Tests: `tools/test_ref_capture.py` (fake adb).

## tools/phone-battery: battery voltage from the bootloader (TP-031)

```
tools/phone-battery [--record RUN_DIR] [--min MV]
```
Only `fastboot getvar product|battery-voltage|battery-soc-ok` (8 s each), only when `product` == `$BOARD`. Last line
`RESULT: PASS battery-voltage=NNNN mV soc-ok=yes|no` (0) / FAIL (1: missing or below `--min`) / ERROR (2). `--record` merges `battery_mv`,
`battery_soc_ok` into `RUN_DIR/run.json`. Tests: `tools/test_phone_battery.py` (fake fastboot). First oriole read: 4029 mV, soc-ok yes [obs 2026-11-14].

### sweetpcie1mmu=1 (xnu patch 0149, READ-ONLY, plan oneshot-2 W4)
With `sweetwlan>=1` and `--wlan-hold`, right after `SWEET WLAN: stage 1 done`: prints `SWEET PCIE1 MMU: ep cmd/status ... bus master on|off`, the RC iATU inbound regions 0..3
(`atu in rN ...`), then `reading s2mpu_hsi2 0x14480000`, `s2mpu_hsi2 CTRL0=...`, `FAULT_STATUS=...`, `done (read-only)`. No store. A hang after the "reading" line means the S2MPU
block is not readable from EL1 (power cycle reverts). Run: `tools/xrun ... --wlan-hold --intent wlan --args 'sweetwlan=2 sweetpcie1mmu=1' --expect 'SWEET PCIE1 MMU: done'`.

### patch 0147 (sweetwlan=4..6 on oriole)
The level-6 firmware pin is a table (bluejay 1764018 B crc 0x1153ccff, oriole 1764091 B crc 0x92567396), the image bound is 1764091 B; levels >= 4 only accept a table identity.
A stop prints `SWEET WLAN: left after stop: atu_r2=.. sys_mem=.. ca7=.. cfg84=..`; a cfg 0x84 restore with the link down is `skipped` (review F1).

## XNU sweetwlan=7: BCM4389 CA7 release + read-only firmware-ready dump (patch 0150, plan oneshot-2 step W5; BUILT, UNRUN, reviewed SAFE)

```bash
tools/xrun --kernel ~/pixelios-artifacts/xnu-local-ef93d09/kernel.development.sweet --board oriole --no-camera --gui --rails --cpus 8 \
  --wlan-hold --wlan-level 7 --wlanfw ~/pixelios-artifacts/wlan-fw/wlanfw-oriole.bin --intent wlan \
  --expect 'SWEET WLAN: (stage 3 level 7 (done|stopped)|fw ready)' --fail-on 'restore=failed' --label o3-w5-l7 --restart 600,bootloader
```

Level 7 = level 6 (image, NVRAM tail, reset vector) plus W15 (release the CA7: RESET_CTL 1, IOCTL 0x3, RESET_CTL 0, IOCTL 0x1, from the halted IOCTL 0x21 only),
W16 (poll the last RAM word for the shared-area pointer, 100 x 50 ms, link precondition before each poll), W17 (read 0x40 bytes of pciedev_shared_t, the console
header and at most 2 KiB of console text, printable bytes only, now and 2 s later). Output: `SWEET WLAN: W15 ...`, `SWEET WLAN: W16 last RAM word ...`,
`SWEET WLAN: fw ready shared=0x... ver=N ...`, `SWEET WLAN: fwcon: <text>`, `SWEET WLAN: stage 3 level 7 done fw ready shared=...`.
Never written: EP Command (bus master), any BAR2 word after W14, rings, doorbells, host_cap. Left after the run: the firmware running (also after a W16/W17 stop),
SYS_MEM out of reset, ATU region 2 enabled, cfg 0x84 = 0; a power cycle resets the chip. Reviews: docs/reviews/0150-wlan-fw-ready-review-fable-1.md (SAFE-AFTER-FIXES),
-2.md (SAFE; G1-G6 L open: glue gate/latch for the release before any later store level, two cite texts, uncovered mutants X2/X3/X8/X9/X11/X15).

## XNU sweetmali=0x100: replay recorded Mesa job chains (patch 0155, plan oneshot-2 step G2; BUILT, UNRUN, review pending)

Captures come from the host Mesa build (docs/research/52, `gpu/capture/run-capture.sh`); pack them in replay order and boot with the container:

```bash
python3 -I tools/pack_gpujob.py ~/pixelios-artifacts/gpu/capture/gpujob-clear-tri.bin ~/pixelios-artifacts/gpu/capture/clear.malijob ~/pixelios-artifacts/gpu/capture/tri.malijob
SWEET_USBLOG_PORT=64738 tools/xrun --kernel <kernel> --board oriole --no-camera --gui --rails --cpus 8 --gpu-handover --intent gpu \
  --gpujob ~/pixelios-artifacts/gpu/capture/gpujob-clear-tri.bin --args 'sweetmali=0x100' \
  --expect 'SWEET MALI: replay tri=(PASS|FAIL)' --stop-on 'SWEET MALI: replay done' --fail-on 'panic\(' \
  --label gpu-o3-g2-replay1 --lock-wait 3600 --restart 600,bootloader --hard-timeout 600
```

`--gpujob FILE` (tools/xrun, tools/build-xnu-image.sh, needs `--gpu-handover`) puts the container in the payload as `/chosen/memory-map GPUJOB`.
`sweetmali=0x100` = bring-up as `sweetmali=1` without registering the SweetMali service, then the replay; with bit 1 or 2 also set the replay is refused.
Output: `SWEET MALI: replay regs ...` (read-only feature registers), `replay: GPUJOB container ok ...`, per job `replay <name>: N chains ...` and
`replay <name>=PASS|FAIL crc=0x...`, then `replay done N/M PASS`. A fault adds the core's `job ... failed JS_STATUS=` / `MMU fault ... FAULTADDR=` lines
and `replay <name>=FAIL chain K wait E jc=... header exception_status=... fault_pointer=...`; the replay stops there.

## XNU sweetmali bit 0x200: Mali JOB/MMU interrupts (patch 0157, plan oneshot-2 step G4; BUILT, UNRUN, review pending)

First run: one CPU, selftest only (doc 35 s.8 item 8):

```bash
SWEET_USBLOG_PORT=64738 tools/xrun --kernel <kernel> --board oriole --no-camera --gui --rails --cpus 1 --gpu-handover --intent gpu \
  --args 'sweetmali=0x202' --expect 'SWEET MALI: irq counts mode=irq job=([2-9]|[1-9][0-9])' --fail-on 'irq attach failed' --fail-on 'panic\(' \
  --label gpu-o3-g4-irq1 --lock-wait 3600 --restart 600,bootloader --hard-timeout 600
```

Lines: `SWEET MALI: irq attach INTID 452 -> 0 ISENABLER14=... IGROUPR14=... IROUTER=...` (and 453), `irq mode on`, the usual
`selftest RESULT js0=PASS js1=PASS`, then `irq counts mode=irq job=N mmu=0 spurious=0 unhandled_spi=0 storms=0 last_unhandled=0`.
Pass = selftest PASS, mode=irq, job >= 2, unhandled_spi 0, storms 0; a small non-zero spurious is a note, not a failure (review 0157 F8; bounded since F2). `mode=poll` is its own outcome (FAIL of this step, but safe). Any attach refusal (`-> 1` precheck, `-> 5` did not stick) leaves `mode=poll` and the
old polling; nothing is written for a refused precheck. Then with the replay: `--gpujob ... --args 'sweetmali=0x300'`, same `replay tri=PASS`.

## G5: dev overlay first runs, then sweetgl_selftest (plan oneshot-2 step G5; PREPARED, UNRUN; writes the iOS root)

Review: `docs/reviews/g5-dev-overlay-first-runs-review-fable-1.md` (SAFE-AFTER-FIXES, fixes F1-F8 applied here). Scheduled by the orchestrator after
all other phone work; stop G5 at the first FAIL (stricter than plan stop H5: `hfs_mountroot failed` in 2 GUI runs in a row ends every root run).
Kernel for every run: `$A/xnu-local-lane-g-63384af/kernel.development.sweet` (stack up to 0155: 0048 /dev/null tee, 0104/0105 SweetMali, 0117 dev
overlay incl. efc1233; NOT 0157). One new thing per boot.

Overlays (not in git) and their archives (`tools/mkdevoverlay.py DIR OUT.bin`, owner 0:0):

| Dir | Content | Archive sha256 |
|---|---|---|
| `$A/gpu/g5/ov1-hello` | `/private/var/dev/hello` = `hello\n` (6 B) | `1f2fb6e9e3444a1b1edde6f77dbfa7129cf19f6447cac8bb79497d2f1a997b86` |
| `$A/gpu/g5/ov3a-marker` | `/usr/local/pixos/marker` = `pixos marker v1\n` (16 B) | `2cb7bef05245fa9c03f976b57722ad1807ed0cbe420fd828cac36e3998515f34` |
| `$A/gpu/g5/ov3b-marker` | `/usr/local/pixos/marker` = `pixos marker v2\n` (16 B) | `a3d1d4e26c87a553fd8583403e1f25338f7c87f8af4ffa46d400937808c9504c` |

**Before each run** (xrun packs `--overlay DIR` itself): re-pack and compare, stop on a mismatch:
`python3 -I tools/mkdevoverlay.py $A/gpu/g5/<dir> /tmp/<dir>.bin && sha256sum /tmp/<dir>.bin` = the value above.

```bash
A=~/pixelios-artifacts; K=$A/xnu-local-lane-g-63384af/kernel.development.sweet
C=(--kernel "$K" --board oriole --no-camera --gui --rails --cpus 8 --lock-wait 3600 --restart 600,bootloader --hard-timeout 600 --expect SBCoverSheet
   --fail-on 'SWEET DEV: (refused|failed|sync error|bad archive|sweetdev=.* but no SWEETDEV|root is not mounted|out of memory|restore .*: no \.sweetorig)'
   --fail-on 'mounting dirty non-journaled root' --fail-on 'SWEET UFS: (refusing write|command 0x2a)' --fail-on 'panic\(' --fail-on 'Kernel trap')
D='SWEET DEV: done:'
# g5-dev0 (F4): RAM disk present, hook inert (first boot with a RAMDisk node next to rd=ufsroot0; no write)
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov1-hello --args 'sweetdev=0' --fail-on 'SWEET DEV:' --label g5-dev0
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov1-hello   --label g5-dev1  --expect 'SWEET DEV: wrote /private/var/dev/hello 6 bytes' --expect "$D 1 written, 0 skipped, 0 refused/failed"
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov1-hello   --label g5-dev2  --expect 'SWEET DEV: skipped /private/var/dev/hello 6 bytes \(identical\)' --expect "$D 0 written, 1 skipped, 0 refused/failed"
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov3a-marker --label g5-dev3a --expect 'SWEET DEV: wrote /usr/local/pixos/marker 16 bytes [0-9]+ ms' --expect "$D 1 written, 0 skipped, 0 refused/failed"
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov3b-marker --label g5-dev3b --expect 'SWEET DEV: wrote /usr/local/pixos/marker 16 bytes \(original kept\)' --expect "$D 1 written, 0 skipped, 0 refused/failed"
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov3b-marker --label g5-dev3c --args 'sweetdev=restore' --expect 'SWEET DEV: restored /usr/local/pixos/marker' --expect "$D 1 written, 0 skipped, 0 refused/failed"
# g5-dev3d (F5): reads the restore back (v1 again), writes nothing
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov3a-marker --label g5-dev3d --expect 'SWEET DEV: skipped /usr/local/pixos/marker 16 bytes \(identical\)' --expect "$D 0 written, 1 skipped, 0 refused/failed"
```

| Run | Proves | Pass (all: `--expect SBCoverSheet`, no fail-on) |
|---|---|---|
| g5-dev0 | payload RAM disk next to `rd=ufsroot0`, hook inert | no `SWEET DEV:` line at all |
| g5-dev1 | first write: new file, new dir `/private/var/dev` | `wrote /private/var/dev/hello 6 bytes`, `done: 1 written, 0 skipped, 0 refused/failed` |
| g5-dev2 | read-back of dev1 from disk | `skipped ... (identical)`, `done: 0 written, 1 skipped, 0 refused/failed` |
| g5-dev3a | new file + two new dirs (`/usr/local` is absent on the image, so `/usr/local` and `/usr/local/pixos`) | `wrote /usr/local/pixos/marker 16 bytes N ms` (no `(original kept)`), `done: 1 written, 0 skipped, 0 refused/failed` |
| g5-dev3b | first replacement (tmp + rename), `.sweetorig` = v1 | `wrote ... 16 bytes (original kept)`, `done: 1 written, 0 skipped, 0 refused/failed` |
| g5-dev3c | `sweetdev=restore` | `restored /usr/local/pixos/marker`, `done: 1 written, 0 skipped, 0 refused/failed` |
| g5-dev3d | the restore reached the disk (file is v1 again) | `skipped ... (identical)`, `done: 0 written, 1 skipped, 0 refused/failed` |

Check in every report that the shim/loader lines do not say `ramdisk overlaps kernel memory`. Optional (review F10): copy the re-packed `/tmp/<dir>.bin`
next to the run's report. Final state after dev0-dev3d (created things are permanent, `restore` removes none, review F7): directories
`/private/var/dev`, `/usr/local`, `/usr/local/pixos`; files `/private/var/dev/hello` (6 B), `/usr/local/pixos/marker` (v1, 16 B); no `.sweetorig`
left after the restore [guess from the restore rename; dev3d only shows the content].
Recovery (F2): as the checklist above; a damaged root is re-flashed by the user only, after confirming the image:
`tools/fflash-userdata.sh $A/ios-root-oriole-v2.img --yes-erase-userdata --force` (`--force`: that image's hash is already in `$A/flashed-userdata.txt`).

g5-st1 (sweetgl_selftest; NOT prepared): `--gpu-handover --intent gpu --args "sweetmali=1 sweetnull=backboardd,SpringBoard,<launcher name>"`, expect
`sweetgl selftest: clear=PASS tri=PASS`, plus the C list.

G5 content still open (lane G stopped; see the G5-READY report): (1) a launcher that waits for the SweetMali service and runs
`sweetgl_selftest` at most once per boot with its output on /dev/null (spec: `$A/gpu/g5/spec/test_sweetgl_once.py`); (2) a host tool that writes
the replacement `/System/Library/xpc/launchd.plist` = the flashed root's cache (extracted read-only from `$A/ios-root-oriole-v2.img`,
sha256 `7aab4ecbdaec394205a955f0dce2abf9dd571614b3aee53eaa7439d455763ede`, 523 LaunchDaemons, no `org.pixelios` job) plus exactly one job
`org.pixelios.sweetgl-selftest` (RunAtLoad, UserName root, no KeepAlive), with a test that the only difference is that job. Files of that run would be
new files only: `/usr/local/bin/sweetgl_selftest`, `/usr/local/lib/libsweetgl.dylib` (20.9 MB: the U1 size ramp applies, research 41), the launcher,
plus the one replacement `launchd.plist` (kept as `.sweetorig`; `sweetdev=restore` puts it back).
One-client rule (0105): with `sweetmali=1` and no `sweetenv`, backboardd does not load libsweetgl and never opens SweetMali, so the self-test is the only
client; the user client accepts root (the job runs as root).

### G5 self-test runs: size ramp, files, launchd job (PREPARED, UNRUN; after g5-dev3d)

Review: `docs/reviews/g5-selftest-runs-review-fable-1.md` (SAFE-AFTER-FIXES; F1-F11 applied here). Same kernel and `C` flag list as the dev runs
above (`xnu-local-lane-g-63384af`, sha256 `f3e3d113…`: 0117 dev overlay, 0104/0105 SweetMali, 0048 /dev/null tee; reviewed 0155, no 0157). Same
re-pack + sha256 check before each run. Read the forwarded `sweetgl_once:`/`sweetgl_selftest:`/`sweetgl:` lines of every st run (review F4: the launcher
retries every `clear=SKIP tri=SKIP` result without a renderer line, also a permanent `dlopen` failure; its "service never appeared" text can be wrong).

| Dir (`$A/gpu/g5/`) | Files (mode, owner 0:0) | File bytes | Archive bytes | Archive sha256 |
|---|---|---|---|---|
| `ov-ramp1` | `/private/var/dev/ramp-1m.bin` 0644 (bytes 0..255 repeated) | 1048576 | 1048656 | `3dfafca0ab5933740b0b4d92bbcaa6fd6816f83a67f2f41b0b0b1070e4e1e1c2` |
| `ov-ramp8` | `/private/var/dev/ramp-8m.bin` 0644 | 8388608 | 8388688 | `78a4ea7eb532818d0d48766404d013a30a3e233d07212905942caa73e73d0452` |
| `ov-ramp24` | `/private/var/dev/ramp-24m.bin` 0644 | 25165824 | 25165904 | `87c8950075161254e8eb4e88f23f07dd7b8ac373d7c1d5091f589be7c4e2fa02` |
| `ov-ramp21` | `/private/var/dev/ramp-21m.bin` 0644 (2026-11-17; archive = ov-st1 size) | 21056960 | 21057040 | `f909f3fb21064c60657c3b1395a84a39ee6f46ccf5619f694734d745da901b16` |
| `ov-ramp16` | `/private/var/dev/ramp-16m.bin` 0644 (2026-11-17; bisect) | 16777216 | 16777296 | `d51233466e19cc4f8b8557d99ca99af2418eea27bc12a3fecfc234056cd706ad` |

**Result 2026-11-17 [obs, PORT.md]:** ramp1/ramp8 PASS; ramp16, ramp21, ramp24 return to fastboot at +7 s with no log. The SWEETXNU
payload (kernel 15936688 B + overlay archive, `tools/mkpayload.py` size) boots at 24325456 B (ramp8) and fails at >= 32714064 B (ramp16).
`libsweetgl.dylib` alone (archive 20920576 B, payload 36857344 B) is over that, so no split of `ov-st1` fits one boot: G5 st is blocked
until the dylib shrinks by ~13 MB or the limit is found and moved (cause unread: the loader message goes to screen/pstore only [guess]).
`llvm-strip` refuses the dylib (load command 0x1e); `__LINKEDIT` is only 2.03 MB, so stripping cannot close the gap [obs].
| `ov-st1` | `/usr/local/bin/sweetgl_once` 0755 (`cf7a9376…`), `/usr/local/bin/sweetgl_selftest` 0755 (`67733d40…`), `/usr/local/lib/libsweetgl.dylib` 0755 (`f2b31a68…`), copies of `$A/gpu/out/` | 21056832 | 21057040 | `69ced5a3bf7c0a0cd3d97c51816d18356678125079e83a0d51ddcba9e9a18795` |
| `ov-st1z` | same three files as `ov-st1` (2026-11-17); packed with `--zlib-above 1048576`: archive version 2, the dylib is a zlib stream of 4719242 B (from 20920496), the two binaries stay stored; needs xnu patch 0163 | 21056832 | 4855840 | `65a76f3fc7d61010a45decce1306b3604426b6ec166e82483f3a0b1be91b2171` |
| `ov-st2` | `/System/Library/xpc/launchd.plist` 0644 = `tools/mk_launchd_job.py $A/gpu/g5/root-ro/launchd.plist OUT --sha256 7aab4ecb…` (523 -> 524 jobs; the job has no UserName key, launchd's default for a LaunchDaemon is root [guess]; file sha256 `7c764fd52774ca0dd3f471ceee565bc812a3f8069c03d023c857dde71d5b24a4`) | 792710 | 792800 | `011ac014b8ef063b65855b5da96920abb92587ccafd2646f16a04602d3adc904` |
| `ov-st2orig` | `/System/Library/xpc/launchd.plist` 0644 = the root's own cache (`7aab4ecb…`): pre-check and restore read-back | 792460 | 792544 | `60fe4a3454ae976e1cb9fbf8dfa39fa2a4013f852087e6c33dd479bb938ec484` |

All paths are inside the 0117 allowlist (`/private/var/dev/`, `/usr/local/`, exactly `/System/Library/xpc/launchd.plist`); every archive is under
the 40 MiB cap; `mkdevoverlay.parse` reads all back with crc ok [obs]. The 24 MiB ramp covers `ov-st1` (21.06 MB archive), the largest RAM disk of G5.
**ov-st1z (2026-11-17, PREPARED, UNRUN):** the way around the payload limit. Patch 0163 (on top of 0117) adds archive version 2: per entry
the stored length, the uncompressed length (<= 40 MiB per entry and in total) and the crc32 of the uncompressed bytes; a zlib entry is inflated by the
kernel's own zlib (`libkern/zlib.h`) in 1 MiB pieces into `<path>.sweettmp`, cut off at the declared length, and renamed only after the stream ended
exactly at the stored length and length + crc match (else the tmp is removed, the target untouched, the entry `refused`); identical-skip compares the
inflated bytes. Version 1 archives (every row above) are byte-identical and behave as before (`ov-st1` re-packs to `69ced5a3…` [obs]). Payload for
g5-st0a with ov-st1z: kernel 16000992 + archive 4855840 + 80 = 20856912 B [calc, as ramp8: 24325456 = 15936688 + 8388688 + 80], under the
24325456 B that booted. Kernel: `$A/xnu-local-g5z/kernel.development.sweet` (sha256 `5ef80d62…`), integrated `wip/oriole-next` (55b547d) + 0163,
label `main`: it holds 0048, 0104/0105, 0117 and 0155 as the 63384af kernel did, the later patches are inert without their boot-args. Host tests:
`tools/test_mkdevoverlay.py`, `tools/test_devoverlay_kernel.py` (sd_stream against host zlib: good, truncated, wrong crc, bomb, caps; 35 mutants caught) [obs].
Runs: the same st order and expects as below, with `K=$A/xnu-local-g5z/kernel.development.sweet`, `--overlay $A/gpu/g5/ov-st1z` in place of
`ov-st1` for st0a/st0b, and `SWEETDEV_ZLIB_ABOVE=1048576` in the environment of every `xrun` (build-xnu-image.sh then packs with `--zlib-above`;
re-pack check: `tools/mkdevoverlay.py --zlib-above 1048576 $A/gpu/g5/ov-st1z OUT` = `65a76f3f…`). The `wrote`/`skipped` lines print the
uncompressed size (20920496), so the st0a/st0b expects are unchanged. The ov-st2/st2orig archives stay version 1 (no file above 1 MiB).
Boot-args of g5-st1b (args-only hook of build-xnu-image.sh, with xrun's `--rw` and remote/usblog args): 520 bytes of the 607-byte budget [obs review].

```bash
# (A, K, C, D as in the dev block above)
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov-ramp1  --label g5-ramp1  --expect 'SWEET DEV: wrote /private/var/dev/ramp-1m.bin 1048576 bytes [0-9]+ ms'  --expect "$D 1 written, 0 skipped, 0 refused/failed"
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov-ramp8  --label g5-ramp8  --expect 'SWEET DEV: wrote /private/var/dev/ramp-8m.bin 8388608 bytes [0-9]+ ms'  --expect "$D 1 written, 0 skipped, 0 refused/failed"
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov-ramp24 --label g5-ramp24 --expect 'SWEET DEV: wrote /private/var/dev/ramp-24m.bin 25165824 bytes [0-9]+ ms' --expect "$D 1 written, 0 skipped, 0 refused/failed"
# three new files; no job references them yet, so nothing executes them
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov-st1 --label g5-st0a --expect "$D 3 written, 0 skipped, 0 refused/failed" \
  --expect 'SWEET DEV: wrote /usr/local/lib/libsweetgl.dylib 20920496 bytes [0-9]+ ms'
# read-back: the three files are on disk (any write = FAIL)
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov-st1 --label g5-st0b --expect "$D 0 written, 3 skipped, 0 refused/failed" --fail-on 'SWEET DEV: wrote'
# pre-check (F6): the phone's live cache is the 7aab4ecb… one this series was built from (any write = FAIL; stop G5 st if it fails)
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov-st2orig --label g5-st-pre \
  --expect 'SWEET DEV: skipped /System/Library/xpc/launchd.plist 792460 bytes \(identical\)' --expect "$D 0 written, 1 skipped, 0 refused/failed" --fail-on 'SWEET DEV: wrote'
```
Then, in **one phone-lock session** (F2: the restore and its read-back are unconditional, also after a PASS, so no later boot of any lane runs the job):
```bash
tools/phone-lock run --owner lane-g --wait 3600 -- bash -c '
# g5-st1a (F3): cache + job + exec of an overlay-written binary + dlopen, NO GPU work (no sweetmali: the service is absent, every try is SKIP)
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov-st2 --label g5-st1a \
  --args "sweetnull=backboardd,SpringBoard,sweetgl_once,sweetgl_selftest" \
  --expect "SWEET DEV: wrote /System/Library/xpc/launchd.plist 792710 bytes \(original kept\)" --expect "$D 1 written, 0 skipped, 0 refused/failed" \
  --expect "sweetgl_selftest: loaded /usr/local/lib/libsweetgl.dylib" --expect "sweetgl_once: gave up after 30 tries" \
  --stop-on SBCoverSheet --stop-on "sweetgl_once: (done|gave up)"
# g5-st1b: the same cache again (read back), then the GPU self-test; one client: sweetmali=1, no sweetenv (backboardd never opens SweetMali)
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov-st2 --gpu-handover --intent gpu --label g5-st1b \
  --args "sweetmali=1 sweetnull=backboardd,SpringBoard,sweetgl_once,sweetgl_selftest" \
  --expect "SWEET DEV: skipped /System/Library/xpc/launchd.plist 792710 bytes \(identical\)" --expect "$D 0 written, 1 skipped, 0 refused/failed" \
  --expect "sweetgl selftest: clear=PASS tri=PASS" --stop-on SBCoverSheet --stop-on "sweetgl_once: (done|gave up)" \
  --fail-on "sweetgl_once: gave up" --fail-on "sweetgl selftest: clear=FAIL|tri=FAIL"
# unconditional: put the root own cache back, then read it back (also if st1a/st1b failed, and before any other lane boots)
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov-st2 --args "sweetdev=restore" --label g5-st-restore \
  --expect "SWEET DEV: restored /System/Library/xpc/launchd.plist" --expect "$D 1 written, 0 skipped, 0 refused/failed"
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov-st2orig --label g5-st-check \
  --expect "SWEET DEV: skipped /System/Library/xpc/launchd.plist 792460 bytes \(identical\)" --expect "$D 0 written, 1 skipped, 0 refused/failed" --fail-on "SWEET DEV: wrote"
'
```
(Inside the session each `xrun` already holds the lock; the `C` array and `$D` must be defined in that `bash -c` too, e.g. by `source` of a small
env file. If st1a fails, skip st1b but still run st-restore and st-check.)

Order and stops: g5-ramp1 -> ramp8 -> ramp24 -> st0a -> st0b -> st-pre -> [st1a -> st1b -> st-restore -> st-check], one per boot, stop at the first
FAIL (the bracket always ends with st-restore + st-check). **Ramp:** check the `N ms` of ramp1: more than ~10 s for 1 MiB (F9: the 600 s restart timer
is armed before the root mount and runs during the hook, so 24 MiB needs > ~45 KiB/s) stops the ramp. A RAM disk the bootloader or loader rejects
does not reach the USB log (F7: `ramdisk overlaps kernel memory` is printed by the shim to the screen/pstore only): it shows as an fboot error or as
`NO DATAGRAM`/no `SWEET DEV` line; then G5 st stops (DEFERRED: smaller dylib or a split overlay). **st1a/st1b** are the only runs with a replaced
boot-critical file on disk. If one does not reach SBCoverSheet, or launchd starts nothing, go straight to st-restore (the hook runs before launchd, so
the restore works even if the new cache is bad), then st-check; a root that still does not boot is the checklist's re-flash case (user only, `--force`).
Outcomes:
* st1a PASS: the four expects + SBCoverSheet: the cache was accepted, the job ran, AMFI let the overlay-written binary exec and `dlopen` the dylib.
  `gave up after 30 tries` is expected here (30 tries x 2 s delay, each try bounded to 30 s; worst case 30 x (30 + 2) s = 16 min, longer than the
  600 s run: bounded all the same [src review F5]).
* st1a: `loaded` missing but `sweetgl_once` lines present = exec ok, `dlopen` refused (library validation, U4); no `sweetgl_once` line at all after
  SBCoverSheet = launchd did not start the job (cache or exec, U4).
* st1b PASS: the three expects; `sweetgl_once: done after N tries` with N small (the service comes up with the GPU probe, ~+118 s [obs G2/G4]).
* st1b `gave up`: SweetMali never answered (not registered, `sweetmali=1` missing, another client): no GPU work ran.
* st1b `clear=FAIL` / `tri=FAIL` / a crash line: the userland driver (G3) on hardware; record the forwarded `sweetgl:` lines; no rerun before a fix.
Unknowns (U4), each with its evidence:
* AMFI and an ad-hoc signed binary written by the overlay: the built-in SWEET AMFI marks every binary on the root filesystem as a platform binary and
  leaves signature/page-hash validation to the kernel (`bsd/kern/sweet_amfi.c:13-20` of the built tree) [src]; the iOS system binaries pass the same
  path [obs: the root boots]. Ours are signed `ldid -S` (CodeDirectory v=20400, sha256) [obs note 52 s.11]. Not yet observed for a file the overlay wrote [guess].
* launchd accepting the modified cache: base boot-args carry `launchd_unsecure_cache=1` [obs, args above; `Using unsecure cache` in 4 run logs, review];
  earlier roots booted edited caches (`pixelios-userland/tools/patch-launchd-plist.py`, `sweet-root-tweaks.py:373`) [src]; `enforce_xpc_cache_code_signing`
  is already off on this root (`sweet-root-tweaks.py:411-415`, ExtensionKit) [src]. Exec from `/usr/local/bin`: no rule found that forbids it [guess].
* `dlopen` of `/usr/local/lib/libsweetgl.dylib` by a platform binary: the dylib is on the root filesystem, so it is a platform binary too [src, same hook] [guess for library validation].
* sweetnull matching: `sweetgl_selftest` is 16 characters (MAXCOMLEN) [guess that p_comm holds all 16]; the launcher forwards the child's lines under its own name anyway.
* Owner (F8): the image's cache is `0:80` mode 0644 [obs review, catalog read]; the overlay writes `0:0` 0644 (one owner per archive). The restore
  renames the original inode back, so `0:80` returns. Whether launchd's cache loader looks at the group is unknown [guess: no].
Leftovers after the series (F11; permanent, `restore` removes nothing the overlay created; tell the owner of the root): `/private/var/dev/ramp-{1m,8m,24m}.bin`
(33 MiB), `/usr/local/bin/sweetgl_once`, `/usr/local/bin/sweetgl_selftest`, `/usr/local/lib/libsweetgl.dylib` (21 MB), directories `/usr/local/bin`,
`/usr/local/lib` (plus the dev-run leftovers listed above). The launchd cache is the original again after st-restore (checked by st-check).
### G6-a fix overlay `ov-g6a` (2026-11-17, PREPARED, UNRUN; docs/research/52 s.12)
`$A/gpu/g5/ov-g6a`: `/usr/local/lib/libsweetgl.dylib` 0755 (20938912 B, sha256 `11f6b32c…`, Mesa patch 0007: w30r attachImage copy fallback,
context take-back fix). Send with `SWEETDEV_ZLIB_ABOVE=1048576`: `tools/mkdevoverlay.py --zlib-above 1048576 $A/gpu/g5/ov-g6a OUT` = 4725360 B
(< 8 MB), sha256 `ca2e3a5536fb1cd60ed4b7456b624bab5db5af6a18fa2a86e2d9b84a08869bbb` [obs, re-pack identical]. Boot-args as g6-a (the GLI map
stays at `/usr/local/gli-slots.txt` from `ov-g6map`). Expect in backboardd: no `without a usable context` flood; `copy fallback` once per w30r
shape; one `not importable ... planes` line per fmt 0x0/b3a8/LA08 shape.
### G6-b fix overlay `ov-g6c` (2026-11-17, PREPARED, UNRUN; docs/research/52 s.13)
`$A/gpu/g5/ov-g6c`: `/usr/local/lib/libsweetgl.dylib` 0755 (20940832 B, sha256 `d3ebb69f…`, Mesa patch 0008: APPLE_fence, CA-format uploads,
shader-object slots 591-633 named) and `/usr/local/gli-slots.txt` 0644 (sha256 `12e23dc9…`). Send with `SWEETDEV_ZLIB_ABOVE=1048576`:
`tools/mkdevoverlay.py --zlib-above 1048576 $A/gpu/g5/ov-g6c OUT` = 4740720 B (< 8 MB), sha256
`02fb5a1db8b0e6872eed76ea9c81404ba739d72f08f4b604b30c284f83b54978` [obs, re-pack identical]. Boot-args as g6-b. Expect: no `slot 595/596/599`
trap lines, one `plane N` line pair for 'b3a8'; the scanout ('b3a8') is still refused, so the screen is not expected to change yet.
### G6-c fix overlay `ov-g6d` (2026-11-17, PREPARED, UNRUN; docs/research/52 s.14)
`$A/gpu/g5/ov-g6d`: `/usr/local/lib/libsweetgl.dylib` 0755 only (20940976 B, sha256 `82c4ebf8…`, Mesa patch 0009: 'b3a8' scanout copy fallback,
plane 0 = w30r encoding ASSUMED). The map stays `/usr/local/gli-slots.txt` from ov-g6c. Send with `SWEETDEV_ZLIB_ABOVE=1048576`:
`tools/mkdevoverlay.py --zlib-above 1048576 $A/gpu/g5/ov-g6d OUT` = 4729968 B (< 8 MB), sha256
`edd73fc9f84da1e2b0fd8d38cac707bde1f380e0ac200643a1bef402ce175848` [obs, re-pack identical]. Expect one `'b3a8' surface ... -> copy fallback`
line with `plane1 ... bpr 1152 (derived ...)`, and `SWEET FPS ... distinct` > 1 if CA's frames now reach the scanout; colours may be wrong.
## XNU `sweettrusty=1`: Trusty version probe (patch 0156, plan step A3)

Two version fastcalls only (`SMC_FC_GET_VERSION_STR`, `SMC_FC_API_VERSION` with 5), as the stock driver's probe does first; no std-call, no
shared memory, no MMIO. Logic and allowlist: `bsd/dev/sweet_trusty.h`, host test `shim/trusty_host_test.c` (plain + ASan in `shim/build.sh`).
```
SWEET_USBLOG_PORT=64738 tools/xrun --kernel ~/pixelios-artifacts/xnu-local-audio-<tag>/kernel.development.sweet --board oriole --no-camera --gui --rails --cpus 8 \
  --args 'sweettrusty=1' --expect 'SWEET TRUSTY: (api=|stop )' --label o5-a3-trusty --restart 600,bootloader --lock-wait 3600
```
Pass: `SWEET TRUSTY: api=5 version='Project: slider, Build: ...'` then `SWEET TRUSTY: probe done smc=N`. A `stop undefined-smc` line means the
monitor does not route Trusty calls from XNU: steps A4-A6 and A8-A10 are skipped (never retried with other arguments, plan stop H9).
Status: *not run yet* (built and host-tested 2026-11-16).
## XNU sweetwlan=8: BCM4389 msgbuf setup + first ioctl round trip (patch 0159, plan oneshot-2 step W7; docs/research/51)

Level 7 (above) plus run F: host_cap, the five common rings in a 256 KiB host pool, EP bus master ON, host-ready doorbell, `cur_etheraddr`
and `ver` through the control rings (polled), guard check, bus master OFF. First step that lets the chip write host RAM.
```
SWEET_USBLOG_PORT=64738 tools/xrun --kernel ~/pixelios-artifacts/xnu-local-w8-<tag>/kernel.development.sweet --board oriole --no-camera --gui --rails --cpus 8 \
  --wlan-hold --wlan-level 8 --wlanfw ~/pixelios-artifacts/wlan-fw/wlanfw-oriole.bin --intent wlan \
  --expect 'SWEET WLAN: (stage 3 level 8 (done|stopped)|fw version)' --fail-on 'restore=failed|DMA suspect|bus_master=ON|bus_master=unknown|restore of group 16 skipped' --fail-on 'panic\(cpu' --fail-on 'Kernel trap at' --label o5-w7-l8 --restart 600,bootloader --lock-wait 3600
```
Pass: `SWEET WLAN: W24 device-to-host DMA works`, `SWEET WLAN: fw version <text> mac xx:xx:xx:xx:<b4>:<b5>`, `stage 3 level 8 done msgbuf round trip ok, bus master off`.
A stop prints `W<row> STOP: <reason>`, the restore lines (EP Command := 0x2 first) and `left after stop: bus_master=...`.
`no completion ... is untouched` = the firmware took the request but nothing reached the host pool (DMA blocked or no answer): the Wi-Fi
ladder ends there, no secure-world call is tried. Left after the run: firmware running, bus master off, pool kept, ATU r2 enabled, cfg 0x84 = 0.
Plan stop H10 applies (a panic outside Wi-Fi code or memory corruption signs in 2 runs ends the bus-master steps).
Status: *not run yet* (built and host-tested 2026-11-16; needs a SAFE review first).
