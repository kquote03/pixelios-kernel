VERDICT: SAFE-AFTER-FIXES

Per run: **g5-dev1, g5-dev2, g5-dev3a: SAFE as prepared** (no change needed to run them, provided the command carries the checklist's `SWEET DEV` fail-on, see F3).
**g5-dev3b, g5-dev3c: SAFE after F1** (a doc-only change of the pass lines; no kernel or tool change). No finding lets any of the five runs damage the root;
the fixes are about detecting a lost write and about a recovery command that would be refused as written. `g5-st1` and the 20.9 MB library overlay are NOT covered.

# G5 dev overlay first runs: safety review (fable 1)

Reviewer: Claude Fable 5.2, 2026-11-16. Independent, read-only on code; nothing was run on the phone. Reviewed: commit `77ced29` (G5 section of
`docs/AUTOMATION.md`), `xnu/patches/0117-sweet-dev-overlay.patch` (P:line = patch-file line), `tools/mkdevoverlay.py`, the three tests, research 41,
reviews 18-20, the overlay archives under `~/pixelios-artifacts/gpu/g5/` (`$A/gpu/g5`), the run kernel `$A/xnu-local-lane-g-63384af/kernel.development.sweet`
(sha256 `f3e3d113cd63...`), the patched tree `$A/localbuild/work/gpu/xnu` (T/; its `bsd/dev/sweet_devoverlay.c` is byte-identical to the patch, observed by `diff`),
and the root image backup. Tags: **[obs]** observed in this session (host), **[src]** read from source, **[guess]**.

## Worst realistic outcome (the plain answer to item 3)

* The five runs do, through the normal VFS/HFS/UFS path, what iOS itself does hundreds of times per boot on this root: mkdir, create a small file, chmod/chown, fsync,
  rename, volume sync [src]. The new, never-executed code is the hook's own sequence of calls, not a new write path into the disk. Every block write still goes through
  `ufs_write_blocks`, which refuses anything outside the `userdata` partition (T/bsd/dev/sweet_ufs.c:303-312) [src]. So docs/SAFETY.md holds.
* **Most likely failure: a panic or hang inside the hook** (first execution of `sd_one`/`sd_parents`/`sd_write_tmp` anywhere; they are not host-tested, review 20 #5).
  Effect: that boot does not reach launchd; the next boot replays the HFS journal (replay is observed on every boot of this root:
  `jnl: b(1, 0): replay_journal ... journal replay done`, run `gpu-o5-base-63384af`, merged.log:153-154 [obs]). Left behind at worst: an empty directory
  (`/private/var/dev`, `/usr/local`, `/usr/local/pixos`) or a stray `PATH.sweettmp`, all inside the allowlist. The root stays bootable without `--overlay`.
* **Worst realistic outcome: a root that no longer mounts** would need a defect in HFS journaling or the UFS driver that the overlay's handful of transactions
  triggers and iOS's own writes (and the hard reset that ends every run) have not triggered so far [guess: unlikely]. Cost: a userdata re-flash by the user
  (flash ~467 s + first-boot migration >= 1050 s per research 41) of an image that exists and matches its hash (item 4) [obs]. Time, not data, as far as the repo
  records what is on that root [guess: nothing of value was created on the phone since the flash].
* No run can touch a file iOS needs: all three target paths are absent on the flashed image [obs], no existing file is opened for writing (the hook only writes
  an exclusively created `PATH.sweettmp`, P:417-418), and the only replacement (dev3b) replaces a file created by dev3a.

## Findings

| id | sev | finding (evidence) | required fix |
|---|---|---|---|
| F1 | M | **The dev3a/3b/3c pass lines cannot detect a lost write.** AUTOMATION.md:689 expects only `wrote /usr/local/pixos/marker 16 bytes` + `done: 1 written` for dev3b. If dev3a's file did not survive (lost before reaching the disk), dev3b creates the file fresh and prints the same `wrote ... 16 bytes` (without ` (original kept)`, P:541) and PASSes; no `.sweetorig` exists and the "first replacement" was never exercised. dev3c (AUTOMATION.md:690) expects only `restored ...`, not the `done:` counts. [src] | Before dev3b: expect `SWEET DEV: wrote /usr/local/pixos/marker 16 bytes \(original kept\)`. All of dev3a/3b/3c: expect the full `SWEET DEV: done: 1 written, 0 skipped, 0 refused/failed`. Exact commands below. |
| F2 | L | **The documented re-flash command is refused as written.** AUTOMATION.md:111-112 says `tools/fflash-userdata.sh IMG --yes-erase-userdata`; the tool refuses an image whose hash is already in `~/pixelios-artifacts/flashed-userdata.txt` unless `--force` (tools/fflash-userdata.sh:15-16), and v2 (`a09f45bb...`, flashed 2026-11-12T14:36:01Z) is recorded there [obs]. | In the recovery text: `tools/fflash-userdata.sh $A/ios-root-oriole-v2.img --yes-erase-userdata --force` (user confirms first, AGENTS.md rule 3). |
| F3 | L | **The run table has no complete commands and no archive hashes.** The rows give overlay + pass text; the fail-on list is included only by the sentence at AUTOMATION.md:693 and differs from the "Common flags" (AUTOMATION.md:679-680 list only `panic\(` and `Kernel trap`). An operator who assembles the command from the common flags alone runs without the `SWEET DEV: (refused|failed|sync error|...)` fail-on. The table carries no sha256 of the archives although `xrun --overlay DIR` re-packs the (writable) DIR at run time (tools/build-xnu-image.sh:55). [obs: `git show 77ced29` has no archive hash] | Put the five full commands (below) and the three sha256 values (item 1) into the G5 section; re-pack and compare the hash right before each run. |
| F4 | L | **dev1 bundles two firsts.** It is the first boot on oriole with a `/chosen/memory-map RAMDisk` node next to `rd=ufsroot0` (research 41 U2: never observed; `md0` appears, T/iokit/bsddev/IOKitBSDInit.cpp:750-766) AND the first write by the hook. "One new thing per boot" (AUTOMATION.md:682) is not met. Not a root risk [src: a misplaced or damaged archive fails the magic/structure/crc checks, P:560-579, P:467], but a dev1 failure would be ambiguous. | Recommended, not required: `g5-dev0` = dev1's command with `--args "sweetdev=0"`. The build keeps an explicit `sweetdev=` (build-xnu-image.sh:48; [obs] boot-args end `... sweetdev=0 sweetremote=1 ...` via `XNU_IMAGE_ARGS_ONLY=1`), and the hook returns silently for any value but `1`/`restore` (P:552-555). Expect SBCoverSheet, `--fail-on "SWEET DEV:"`, no `ramdisk overlaps kernel memory`. |
| F5 | L | **The restore result is never read back.** dev3c proves only that the rename returned 0. Nothing checks that the file is v1 again or that the restore reached the disk. | Add `g5-dev3d`: `--overlay ov3a-marker`, expect `SWEET DEV: skipped /usr/local/pixos/marker 16 bytes \(identical\)` and `done: 0 written, 1 skipped, 0 refused/failed`. It writes nothing and doubles as the health boot after the series. |
| F6 | L | **Fail-on gaps (slow detection only).** `SWEET DEV: restore PATH: no .sweetorig` (P:486) counts as "skipped" and matches no fail-on: dev3c would then wait out the timeout before FAIL. A failed WRITE(10) that surfaces only in the journal's own flush prints `SWEET UFS: command 0x2a failed/timed out` or `refusing write` (T/bsd/dev/sweet_ufs.c:237, 277, 309) and matches no fail-on. (`SWEET UFS: command 0x00 failed` is normal on every boot, merged.log:119 [obs]: do not fail on that.) | Add `restore .*: no \.sweetorig` to the `SWEET DEV` alternation and `--fail-on "SWEET UFS: (refusing write|command 0x2a)"`. |
| F7 | L | **dev3a creates two directories, not one, and created things are permanent.** On the flashed image `/usr/local` does not exist [obs: catalog walk of the backup], so dev3a creates `/usr/local` and `/usr/local/pixos` (allowed: `mkmin` = 10, P:304, P:366-368); dev1 creates `/private/var/dev`. `restore` removes none of them nor the two files (AUTOMATION.md:113 says so for the general case). After the series the root permanently holds: `/private/var/dev/hello`, `/usr/local/pixos/marker` (v1), three directories. Harmless [guess: an empty `/usr/local` changes nothing for iOS 16; dyld only falls back to `/usr/local/lib` when a library is missing]. | Correct the dev3a row ("creates `/usr/local` and `/usr/local/pixos`") and tell the owner of the root the final file list. |
| F8 | Info | **Review 20's durability argument is wrong in one step; the conclusion still holds.** It says each file's `VNOP_FSYNC(MNT_WAIT)` "goes through the same journal flush (hfs_vnops.c:3234-3240)". That block runs only for a full fsync (`if (!fsync_default)`, T/bsd/sweet_hfs/core/hfs_vnops.c:3231); `hfs_vnop_fsync` passes mode 0 (T/...hfs_vnops.c:7358). So the per-file fsync pushes data and the catalog record but does not flush the journal or the device cache. Durability of the create/rename comes only from the `VFS_SYNC` at P:599-604 -> `hfs_sync` -> `hfs_flush(HFS_FLUSH_JOURNAL)` (hfs_vfsops.c:2501) -> `journal_flush` -> `write_journal_header`, which sends SYNCHRONIZE CACHE **before** the header write and not after it (hfs_journal.c:467-482; the after-flush at :523 is only for `updating_start`). The last header can therefore sit in the UFS device cache until the next flush; the driver's log thread flushes the cache every few seconds (sweet_ufs.c:969) and iOS's own transactions follow. [src] Consequence: always consistent; a reset within a few seconds of `SWEET DEV: done` could lose the whole change (never half of it: data is written before the journal header that makes it visible). | None in code. Keep `--expect SBCoverSheet` in every run so the reset comes tens of seconds after the hook (xrun stops and resets as soon as all expects are seen, tools/xrun:50-51); never run these with only `SWEET DEV` expects. dev2 / F1 / F5 are what prove the data reached the disk. |
| F9 | Info | **One code change postdates reviews 18-20:** efc1233 (2026-10-25) replaced the boot-arg parse by `PE_parse_boot_arg_str("sweetdev", arg, sizeof(arg))` after a hardware boot ("g28") where the hook was silently inert. Reviewed here: correct API (T/pexpert/gen/bootargs.c:200-206, forces string), `restore` fits `arg[16]`, P:548-555 [src]; and it is in the run kernel [obs, item "kernel"]. So "never run on hardware" means: the hook was entered once and returned before doing anything; the write path has never executed, on hardware or on a host. | None. |
| F10 | Info | The root backup is a single copy on the same disk as the worktrees (`/dev/nvme0n1p4`, 40 G free) [obs]. xrun does not store the packed archive in the run directory (review 18 #11, still open). | Optional: copy `*.bin` next to each run's report by hand. |

## Per-item verdicts

### Kernel under test: contains 0117, including the review fixes: PASS [obs]
* `grep -a -c 'SWEET DEV' $K` = 19; `strings` shows all 19 format strings of the current patch, incl. the review-19/20 ones (`name too long`, `.sweetorig is not a regular file`,
  `sync error %d`, `ORIGINAL ONLY AT .sweetorig (rollback failed)`), `%s.sweettmp`, `%s.sweetorig`, and the five allowlist strings.
* `llvm-objdump -d --disassemble-symbols=_sweet_devoverlay_run`: calls `_PE_parse_boot_arg_str` (efc1233), `_mdevgetrange`, `_vfs_isrdonly`, two `_snprintf` with
  `mov w1, #0x3f8` (= 1016 = `sizeof(sd_w.tmp)`: review 19 C1 is fixed in the binary) and `cmp w0, #0x3f8`, `_sd_write_tmp`, 4x `_sweet_vfs_rename`, `_unlink1`, `_VFS_SYNC`.
* `git diff 63384af 77ced29 -- xnu/patches/0117-sweet-dev-overlay.patch tools/mkdevoverlay.py tools/build-xnu-image.sh tools/mkpayload.py tools/xrun shim/loader.c`: empty;
  `efc1233` is an ancestor of `63384af`. The worktree's uncommitted changes do not touch 0117 or the overlay tools (`git status`).
* Stack frames in the binary: `_sweet_devoverlay_run` 0x180, `_sd_write_tmp` 0x1f0, `_sd_parents` 0x70, `_renameat_internal` 0x1e0, `_mkdir1at` 0x1c0 (review 18 #2 concern is gone).

### 1. Archive contents and allowlist: PASS [obs + src]
Every archive is 80 bytes, header `SWEETDEV`, version 1, count 1; parsed with `mkdevoverlay.parse` (the kernel twin) and by `xxd`:

| archive (sha256) | path | mode | uid:gid | size | sha256 of data | data | crc |
|---|---|---|---|---|---|---|---|
| `ov1-hello.bin` `1f2fb6e9e3444a1b1edde6f77dbfa7129cf19f6447cac8bb79497d2f1a997b86` | `/private/var/dev/hello` | 0644 | 0:0 | 6 | `5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03` | `hello\n` | ok |
| `ov3a-marker.bin` `2cb7bef05245fa9c03f976b57722ad1807ed0cbe420fd828cac36e3998515f34` | `/usr/local/pixos/marker` | 0644 | 0:0 | 16 | `24c6c2a40d55f3fe52da5d89f1ec48387977fae2b9d064cacb74aa269e4c0b73` | `pixos marker v1\n` | ok |
| `ov3b-marker.bin` `a3d1d4e26c87a553fd8583403e1f25338f7c87f8af4ffa46d400937808c9504c` | `/usr/local/pixos/marker` | 0644 | 0:0 | 16 | `b7b22e02c90032938e2555ca042823e6720a23f79228ed051022e997abd6daac` | `pixos marker v2\n` | ok |

* Source directories hold exactly one regular file each (mode 0644), no symlinks, no other entries (`find -printf`). Re-packing each DIR gives the same bytes (`cmp`, item "Commands").
  The committed table has no hashes to compare with (F3); the values above are the reference.
* Allowlist (P:125-129, `sd_path_check` P:249-309) [src]: both paths are absolute, printable ASCII, have no empty / `.` / `..` component (refused at P:267-275; there is
  no normalisation, such paths are refused, not resolved), no trailing slash (an empty last component is refused, P:267), no reserved suffix, and match `K_DIR`
  prefixes `/private/var/dev/` and `/usr/local/`. `path_check` returns None for both [obs]. Relative paths: `not absolute` (P:253).
* Symlinked components on the real root [src]: every prefix is looked up with `VNODE_LOOKUP_NOFOLLOW` and must be `VDIR` (`sd_parents` P:352-382, `sd_stat` P:334-349);
  a symlink gives `refused ... (parent is not a directory (symlink?))`, nothing written. The target must be absent or `VREG` (P:500); the temp file is created
  `O_CREAT|O_EXCL|O_NOFOLLOW` after an unlink (P:417-418); rename does not follow the last component.
* The flashed image itself [obs, read-only catalog walk of `ios-root-oriole-v2.img`]: `/private` and `/private/var` are real directories (0755 root); `/var` is the
  symlink and is not used; `/private/var/dev` is absent; `/usr` is a directory, `/usr/local` is absent; no file with `sweettmp`/`sweetorig` in its name exists.
  Directory creation: `/private/var/dev` only (`mkmin` 16); `/usr/local`, `/usr/local/pixos` (`mkmin` 10); mode 0755 (P:368). Not verified: the phone's live root (it has diverged since the flash).

### 2. Run order limits damage: PASS, with F4/F7 [src]
dev1: mkdir + one new file. dev2: read-only compare (`sd_same` P:391-410), no write, no sync (P:599). dev3a: two mkdirs + one new file under `/usr/local/`.
dev3b: first replacement, of the file dev3a created (two renames, P:527-533; the window between them leaves only `marker.sweetorig`, known limit F3 of the checklist, harmless here).
dev3c: `sweetdev=restore` renames `marker.sweetorig` over `marker` (P:484-498). No run names a path that exists on the flashed image. Each run changes one thing, except dev1 (F4).
[obs] boot-args built for dev1 end `... ufsrw=1 rootrw=1 ... sweetrestart=600,bootloader sweetremote=1 sweetdev=1 ...`; for dev3c `... sweetdev=restore sweetremote=1 ...` (one `sweetdev=`).

### 3. The kernel hook: PASS (no finding that can damage the root) [src unless tagged]
* Root filesystem: HFSX, case-sensitive (catalog `keyCompareType` 0xBC), journaled (volume attributes 0x80002100, journal info block 129), 4 KiB blocks, 4.3 GiB free [obs on the backup image].
  On the phone the journal is active: replay lines on every mount and `/dev/ufsroot0 on / (hfs, local, journaled, noatime)` (merged.log:153-155, 398) [obs].
* Writes outside the target file: only (a) `mkdir` of missing parents at or below the allowlisted prefix (P:366-368), (b) `PATH.sweettmp` (unlinked first, then created exclusively), (c) `PATH.sweetorig` by rename. All three names are inside the allowlisted directory. A stale `PATH.sweettmp` of any type is removed or makes the entry fail; a directory there gives `failed`.
* Truncation / partial writes: the target is never opened for writing. New content goes to the temp file, is chmod/chown'ed and fsynced, then renamed (P:412-445, 525-533). On any error the temp file is removed and a moved original is renamed back, with the rollback result logged (P:534-539).
* `.sweetorig` collisions: a backup is taken only when none exists; an unreadable or non-regular `.sweetorig` refuses the entry (P:510-519). Not relevant to these paths (no such names on the image).
* Read-only root: `vfs_isrdonly` -> `root is not mounted read-write ... nothing done` (P:580-584); xrun always passes `--rw` (tools/xrun:31).
* Same overlay on a second boot: size + byte compare -> `skipped`, no write and no sync (P:520-524, 599).
* Sync and ordering: see F8. Data blocks -> journal transaction -> SYNCHRONIZE CACHE (implemented: sweet_ufs.c:321-325, 527-535) -> journal header.
* Timed restart / remote reset: `sweet_machine_reset` resets at once, with no sync (T/bsd/dev/sweet_restart.c:57-67, 104-112, 147-153). That is how every run on this root already ends, with iOS writing; the journal replay at the next mount is observed and has not failed [obs for the last runs]. The overlay adds a few transactions at the quietest moment of the boot (single thread, no user process). The 600 s timer cannot fire during the hook; the host's reset comes only after all expects or a fail-on, and the log reaches the host only from about +118 s (usbnet first line, report.txt), long after the hook and its sync.
* HFS/UFS at that point of boot: after `vfs_mountroot`, `set_rootvnode`, devfs and `siginit`, on the `bsd_init` thread with `vfs_context_kernel()` (T/bsd/kern/bsd_init.c:1046-1050). The port's MAC policies register no vnode create/rename/unlink hooks (0050: only `mpo_vnode_check_signature`, `mpo_policy_syscall`), so nothing inspects a process that does not exist. `mkdir1at`/`renameat_internal` are the unmodified syscall bodies called with `UIO_SYSSPACE` (T/bsd/vfs/vfs_syscalls.c:8929, 9719, 9906-9917). The UFS driver is synchronous in the caller's thread (review 18).
* Vnode references: every `vnode_lookup` has its `vnode_put` (P:338-348), every `vnode_open` its `vnode_close` (P:408, 443). The root is never unmounted anyway.
* Not covered by any test or earlier run: the whole write path (F9). A bug there shows as `failed`/`refused`, a wrong count in `done:`, a panic, or a hang; the commands below turn each into a FAIL.

### 4. Recovery: PASS, with F2
| failure | documented recovery (AUTOMATION.md:111-114) | needs a re-flash? |
|---|---|---|
| panic / Kernel trap / hang in dev1 or dev2 | boot the same kernel without `--overlay` (hook inert, journal replays) | only if that boot does not reach the GUI |
| `refused` / `failed` / wrong `done:` counts | stop G5; nothing or only temp/dirs were written; a plain boot shows the state | no |
| dev3b interrupted between its two renames (marker absent, `.sweetorig` = v1) | same DIR with `--args "sweetdev=restore"`, or the same overlay again | no (and the marker is not needed by iOS) |
| dev3c says `no .sweetorig` | nothing to do; the marker stays v2 | no |
| `hfs_mountroot failed` / `cannot mount root` (xrun_report.py:49, 425 scores it FAIL by itself) | none on the phone; twice in a row = plan stop H5 | **yes**, by the user: `tools/fflash-userdata.sh $A/ios-root-oriole-v2.img --yes-erase-userdata --force` (F2) |
| `mounting dirty non-journaled root` | stop; the journal is gone | likely yes |

Backup [obs]: `~/pixelios-artifacts/ios-root-oriole-v2.img` (17179869184 bytes, 12 G on disk) and its `.sha256` exist; `sha256sum` =
`a09f45bb6dcfc95905fad4e25636ac4b73885e51a6d55ea4739d5fab6f81a19a` = the `.sha256` file = the last line of `flashed-userdata.txt` (flashed 2026-11-12T14:36:01Z).
The image is clean (unmounted bit set, last mounted by `fsck`). A re-flash loses what the phone wrote since the flash (migration state, logs), i.e. it costs the flash and one migration boot.

### 5. Pass lines and fail-on: PASS for dev1/dev2; dev3a-c need F1; F3/F6
* dev1/dev2 (checklist lines 107-109): `wrote ... 6 bytes` + full `done:` counts + GUI marker, then `skipped ... (identical)` + `done: 0 written, 1 skipped`. A partial copy cannot produce these: `wrote` is printed only after the final rename returned 0 (P:541), and dev2's `skipped` requires the same size and bytes read back from the disk after a reboot (P:520).
* The fail-on alternation matches every failure printf of the patch: `refused` (P:471, 476, 489, 501, 511, 516), `failed` (P:492, 506, 537), `sync error` (P:602), `bad archive` (P:573, 577), `sweetdev=.* but no SWEETDEV` (P:566), `root is not mounted` (P:582), `out of memory` (P:587); it does not match the `done:` line. Missing: `restore ...: no .sweetorig` (F6).
* Log delivery: the lines between `hfs: mounted iOSRoot` and `load_init_program` reach the host (merged.log:155-158 of the baseline run [obs]); the hook prints there.
* H5 (`hfs_mountroot failed` in 2 consecutive GUI runs ends all root-using runs; plan `2026-11-14_oriole-oneshot-2.md`:73): the G5 section's "stop G5 at the first FAIL" (AUTOMATION.md:682) is stricter, and xrun scores the marker FAIL on its own. Sufficient; citing H5 in the section would help.

### 6. Open items of reviews 18-20: all conditions closed [src + obs]
| item | state at 77ced29 |
|---|---|
| r18 #1 O_EXCL temp, #2 buffers off the stack, #5/#6, #7, #9, #10, #12 | closed (P:417-418, P:452-454, P:510-519, P:535-537, P:229-246; mkdevoverlay.py `collect`) |
| r18 #3 first run = one new file | closed (AUTOMATION.md:105-110; the G5 table follows it) |
| r18 #4 kernel functions compiled on the host | closed for the pure functions (`test_devoverlay_kernel.py`, 4 tests OK with sanitizers [obs]); the write path is not (r20 #5, accepted) |
| r18 #8 / r19 L3 volume sync | closed (P:599-604); see F8 |
| r18 #11 record of what a run wrote; no delete | open, not a safety condition (F10, F7) |
| r19 C1 truncated temp/backup names | closed in source (P:311-318, 475) and in the binary (`#0x3f8`) [obs] |
| r19 M1-new, L1, L2, L4, L5 | closed (test:55, 104, 176-185; P:515-519; P:258; AUTOMATION.md:107) |
| r20 #1 static check region | closed (test_devoverlay_kernel.py:202-203 split on the exact end marker, count == 1) |
| r20 #2 sync on `sd_nw \|\| sd_nr`, `sync error` in fail-on | closed (P:599; AUTOMATION.md:104, 107) |
| r20 #3 K_NAME exact prefix | closed (test:104 corpus, :179 mutant) |
| r20 #4 `VFS_SYNC` without `vfs_busy`, #5 no host coverage of the write path | open by decision (info) |
| "review 0117 F1-F5" (AUTOMATION.md:114) | F1 closed (build-xnu-image.sh:47, test); F3-F5 are documented limits. That review file is not in `docs/reviews/` of this worktree: not read. |

### 7. U1 (ramdisk size): not exercised by these runs
The three archives are 80 bytes each [obs]; the payload segment is one 16 KiB unit [src mkpayload, review 18]. The same "extra payload segment" mechanism already ran on
oriole for the 0155 GPU job container (`gpu-o5-g2-replay1` PASS, per commit c581e3d; its log was not re-read here), under another ADT name. **The 20.9 MB `libsweetgl.dylib`
overlay of `g5-st1` is NOT covered by this review**: U1 (largest ramdisk the bootloader accepts, placement against the kernel range) needs its own size ramp, and so
do the `launchd.plist` replacement and U4 (AMFI/dyld accepting files written this way; these five runs write text files that nothing executes).

### 8. Other
* F8 (durability reasoning), F9 (post-review change, earlier inert boot), F4 (U2 bundled into dev1), F7 (permanent leftovers).
* `IO_NODELOCKED` etc.: nothing new beyond review 18.
* The checklist precondition "the owner of the installed root has been told" is a process step; not checkable here.

## Proposed exact commands (not run; regexes checked against the printf strings by reading)

```bash
A=~/pixelios-artifacts; K=$A/xnu-local-lane-g-63384af/kernel.development.sweet
C=(--kernel "$K" --board oriole --no-camera --gui --rails --cpus 8 --lock-wait 3600 --restart 600,bootloader --hard-timeout 600 --expect SBCoverSheet
   --fail-on 'SWEET DEV: (refused|failed|sync error|bad archive|sweetdev=.* but no SWEETDEV|root is not mounted|out of memory|restore .*: no \.sweetorig)'
   --fail-on 'mounting dirty non-journaled root' --fail-on 'SWEET UFS: (refusing write|command 0x2a)' --fail-on 'panic\(' --fail-on 'Kernel trap')
D='SWEET DEV: done:'
# optional g5-dev0 (F4): hook inert, RAM disk present
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov1-hello --args 'sweetdev=0' --fail-on 'SWEET DEV:' --label g5-dev0
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov1-hello   --label g5-dev1  --expect 'SWEET DEV: wrote /private/var/dev/hello 6 bytes' --expect "$D 1 written, 0 skipped, 0 refused/failed"
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov1-hello   --label g5-dev2  --expect 'SWEET DEV: skipped /private/var/dev/hello 6 bytes \(identical\)' --expect "$D 0 written, 1 skipped, 0 refused/failed"
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov3a-marker --label g5-dev3a --expect 'SWEET DEV: wrote /usr/local/pixos/marker 16 bytes [0-9]+ ms' --expect "$D 1 written, 0 skipped, 0 refused/failed"
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov3b-marker --label g5-dev3b --expect 'SWEET DEV: wrote /usr/local/pixos/marker 16 bytes \(original kept\)' --expect "$D 1 written, 0 skipped, 0 refused/failed"
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov3b-marker --label g5-dev3c --args 'sweetdev=restore' --expect 'SWEET DEV: restored /usr/local/pixos/marker' --expect "$D 1 written, 0 skipped, 0 refused/failed"
# g5-dev3d (F5): restore read back, writes nothing
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" --overlay $A/gpu/g5/ov3a-marker --label g5-dev3d --expect 'SWEET DEV: skipped /usr/local/pixos/marker 16 bytes \(identical\)' --expect "$D 0 written, 1 skipped, 0 refused/failed"
```
The dev3a expect `16 bytes [0-9]+ ms` rejects an unexpected `(original kept)` (a marker that already existed). Check in every report that the shim/loader lines do not say
`ramdisk overlaps kernel memory` (AUTOMATION.md:108). Stop at the first FAIL.

## Commands run in this review, with results [obs]

| command | result |
|---|---|
| `pixos-run.sh 'python3 -B tools/test_mkdevoverlay.py'` | `Ran 20 tests in 0.302s` / `OK` |
| `pixos-run.sh 'python3 -B tools/test_devoverlay_kernel.py'` | `Ran 4 tests in 6.742s` / `OK` (no skips: gcc + sanitizers present) |
| `pixos-run.sh 'python3 -B tools/test_build_overlay_args.py'` | `Ran 8 tests in 0.570s` / `OK` |
| `python3 -I tools/mkdevoverlay.py $A/gpu/g5/ov1-hello /tmp/claude-1000/ov1.bin` (and `ov3a-marker` -> ov2, `ov3b-marker` -> ov3) | `1 files, 80 bytes` each; sha256 `1f2fb6e9...`, `2cb7bef0...`, `a3d1d4e2...`; `cmp` with the stored `.bin`: identical, all three |
| `sha256sum ~/pixelios-artifacts/ios-root-oriole-v2.img` | `a09f45bb6dcfc95905fad4e25636ac4b73885e51a6d55ea4739d5fab6f81a19a` = `.sha256` file |
| `grep -a -c 'SWEET DEV' $K`; `strings`; `llvm-nm`; `llvm-objdump` of the hook | 19; see "Kernel under test" |
| `diff` of the patch's `sweet_devoverlay.c` against `$A/localbuild/work/gpu/xnu/bsd/dev/sweet_devoverlay.c` | identical |
| `XNU_IMAGE_ARGS_ONLY=1 tools/build-xnu-image.sh --board oriole ... --overlay DIR [--args ...]` (test hook: no build, no phone) | boot-args as quoted in items 2 and F4 |
| read-only parse of the backup image's HFS+ volume header and catalog B-tree (own script, `mmap` read-only) | signature `HX`, journaled, clean; path facts of item 1 |

The worktree had uncommitted changes in four unrelated files (`docs/AUTOMATION.md` G4 lines, `shim/sweet_spi_host_test.c`, `tools/test_sweet_spi_mutants.py`, `0157`); none
affects the three tests, so they were run in place. Temporary files were written only under `/tmp/claude-1000/`.

## Not verified

* Anything on the phone: the live root's state (whether `/private/var/dev` or `/usr/local` appeared since the flash; a symlink there would be refused, not followed), the UFS write-cache setting, behaviour of the device cache across the PMU software reset [guess: kept, the device stays powered].
* That the kernel binary was built from exactly `63384af` (no build record was read); what was checked is that its strings and disassembly match the current patch.
* U1, U2 (first observed by dev0/dev1), U4, and everything about `g5-st1`.
* The proposed commands were not executed, not even as a dry run; `tools/run-preflight` may want an `--intent`.
* XNU internals below `renameat_internal`/`mkdir1at`/`vn_open` beyond what is cited; I relied on them being the stock syscall paths that iOS userland exercises on this root in every boot [obs for the paths, guess for "no kernel-context difference"].
* The file of "review 0117 F1-F5" (not in this worktree).
