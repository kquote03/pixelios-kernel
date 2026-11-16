VERDICT: SAFE-AFTER-FIXES

Per run: **g5-ramp1, g5-ramp8, g5-ramp24, g5-st0a, g5-st0b: SAFE as prepared** (they create new files only; F6/F7 are optional detection improvements).
**g5-st1: SAFE after F1 and F2** (two doc-only changes: one more `--stop-on`, and the restore is not optional); F3 (split it into two boots) is strongly recommended, not required.
**g5-st-restore, g5-st-check: SAFE as prepared** (F6 optional). No run should be dropped. No finding lets a run damage the root beyond what `sweetdev=restore` undoes;
the replacement cache is, apart from the one job, byte-for-byte what the same serialiser already produced for the cache this root boots with.
Precondition unchanged: g5-dev0 ... g5-dev3d have run and passed.

# G5 self-test runs: safety review (fable 1)

Reviewer: Claude Fable 5.2, 2026-11-16. Independent, read-only; nothing was run on the phone, no kernel or Mesa build. Reviewed: commit `e98c327` (worktree clean),
section "G5 self-test runs" of `docs/AUTOMATION.md` (lines 741-800; A:n = line), `xnu/patches/0117-sweet-dev-overlay.patch` (P:n), `tools/mkdevoverlay.py`,
`gpu/sweetgl-once/sweetgl_once.c` (L:n), `tools/mk_launchd_job.py`, their tests, `xnu/patches/0048`, `0104`, `0105`, the built tree's `bsd/kern/sweet_amfi.c`
(`$A/localbuild/work/gpu/xnu`), `gpu/mesa-patches/0006` (self-test and frontend source), `tools/xrun`, `tools/xrun_report.py`, `tools/build-xnu-image.sh`,
`shim/loader.c`, `shim/shim.c`, `pixelios-userland/tools/sweet-root-tweaks.py`, the six overlay dirs/archives in `$A/gpu/g5` (`$A` = `~/pixelios-artifacts`),
the root image backup, and the logs of four earlier oriole runs (`logs/runs/`). Tags: **[obs]** observed here on the host, **[src]** read from source, **[guess]**.

## Findings

| id | sev | finding (evidence) | required fix |
|---|---|---|---|
| F1 | M | **g5-st1 can be cut off before SBCoverSheet, right after launchd started.** An explicit `--stop-on` replaces the default "stop when every `--expect` is seen" (`tools/xrun`: `stops=rawexp` only when no `--stop-on` was given; `xrun_report.py` waitusb: `if stop and stop_seen(d, stop)` -> EARLY -> software reset) [src]. A:774 gives only `--stop-on 'sweetgl_once: (done\|gave up)'`. In the baseline run with this kernel (`gpu-o5-base-63384af`) launchd's first line is in the first log batch at +118 s and SBCoverSheet is found at +139 s [obs]; the GPU core is up before launchd (merged.log:151 vs :160), so the launcher's first try can print `done` within a few seconds of launchd [guess for the self-test's duration]. Effects: (a) a good run is scored FAIL (`SBCoverSheet` missing); (b) the reset comes seconds, not tens of seconds, after the hook's rename of the boot-critical file (review dev-overlay F8: the last journal header may sit in the UFS cache until the next flush). (b) cannot half-write the cache [src F8: lost entirely or not at all], but it makes the one critical run ambiguous. | In the g5-st1 command add `--stop-on SBCoverSheet` (both stop patterns must be seen). Nothing else changes: `gave up` and `FAIL` still end the run through `--fail-on`. |
| F2 | M | **"unless the orchestrator wants the job kept" (A:786) leaves a root that behaves differently in every later boot, of every lane.** With the job in the cache the launcher starts at each boot: without `sweetmali` it runs the self-test 30 times (30 x `dlopen` of 20.9 MB + 2 s) [src L:28-30, L:221-237]; in any other run whose `sweetmali` has bit 0 or 1 (the G4 commands `sweetmali=0x202`, A:667; any `sweetmali=1/2/3`) the service registers (0105 `(mode & 3u) == 0` -> not started) and the job opens the single user client and submits Mesa job chains while that run's own kernel self-test / IRQ test uses the GPU, or takes the one-client token from a later userland client [src 0105 start(), initWithTask]. The phone and its root are shared (phone lock queue). | Make the restore unconditional: "after g5-st1, whatever its result, run g5-st-restore then g5-st-check before the phone lock is released for any other run". Drop the "unless ..." clause (keeping the job is a new decision with its own review). |
| F3 | L (recommended) | **g5-st1 writes one file but exercises five things for the first time in one boot:** launchd accepting a cache with an added job whose plist path does not exist on disk; exec of a binary the overlay wrote (U4); `dlopen` of the 20.9 MB dylib (U4); the first open of the SweetMali user client on oriole (no `SWEET MALI: uc ` line and no open in any oriole run log in `pk-gpu/logs/runs` or `pk-dev/logs/runs`; G2 ran `0x100` = no service, G4 registered the service but nothing opened it) [obs]; the first Mesa-built chains. A kernel panic in the GPU part (0105 itself records an earlier `phystokv_range` panic in `IOConnectMapMemory64`, comment at the `gPhysBase` extern) would hit while the replaced cache is on disk and be indistinguishable from a cache problem at first sight. Not a root risk [src: restore runs before launchd]. | Recommended: split. **g5-st1a** = the st1 command without `sweetmali=1` (keep `--gpu-handover --intent gpu` or drop both): the service is absent, every try prints `SKIP`. Expect `wrote ... 792710 bytes \(original kept\)`, the `done:` counts, `sweetgl_selftest: loaded /usr/local/lib/libsweetgl.dylib`, `sweetgl_once: gave up after 30 tries`, SBCoverSheet; no `--fail-on 'gave up'`. That proves cache + job + AMFI exec + dlopen with no GPU access. **g5-st1b** = today's st1 with the DEV expects changed to `skipped /System/Library/xpc/launchd.plist 792710 bytes \(identical\)` and `done: 0 written, 1 skipped, 0 refused/failed`. |
| F4 | L | **The launcher's "GPU work at most once" has a gap, and its give-up text can be wrong.** Rule 1 retries every try that ends `clear=SKIP tri=SKIP` without a `GL_RENDERER=` line (L:231). The self-test prints exactly that also when (a) `dlopen` fails (permanent: AMFI/library validation), (b) `sweetgl_init` fails *after* `IOServiceOpen` succeeded (`panfrost_drm_screen_create failed`), (c) context creation fails (`no context`) [src mesa patch 0006 `sweetgl_selftest.c` main, `sweetgl_frontend.c` `sweetgl_init`]. In (b)/(c) the user client is opened, queried and may map BOs (GPU MMU updates) up to 30 times; no job chain is submitted (the first draw is after the renderer line) [src; guess that Panfrost screen/context creation submits nothing]. In (a) the last line says "the SweetMali service never appeared". Bounded (30 tries, each try's kernel work is torn down at exit, 0105 `teardown`). | None before the runs. Read the forwarded `sweetgl_selftest:` / `sweetgl:` lines, not only the `gave up` text. Later: retry only when the try printed `no SweetMali service`. |
| F5 | L | **Text does not match the source.** A:789 "40 tries x 5 s": the binary has 30 tries, 2 s delay, 30 s per try (L:28-30; `mov w8,#0x1e`, `#0x7530` in the disassembly [obs]). Worst-case launcher lifetime is therefore 30 x (30 + 2) s = 16 min, longer than the 600 s run (bounded all the same). A:735 "UserName root": the job has no `UserName` key (launchd then runs it as root; 124 of the 523 existing jobs have none [obs]). A:751 "21057040 total" is the archive size; the three files are 21056832 bytes. A:757 "473 bytes": I get 520 with the flags xrun adds (limit 607) [obs]. L:2 and the test cite "docs/research/52 s.12"; that document ends at s.11.6 [obs]. | Correct the five texts. |
| F6 | L | **The read-back runs are not read-only when the content differs, and nothing proves beforehand that the phone's cache is the `7aab4ecb...` one.** `ov-st2orig` under `sweetdev=1` replaces a differing `launchd.plist` (P:520-533) and prints `wrote ... 792460 bytes (original kept)`, which matches no fail-on: g5-st-check would end at the timeout having replaced the cache. Same shape for g5-st0b. The base cache is proven identical to the **image backup** (item 2) and the image is the last flash in `$A/flashed-userdata.txt`; the phone's live file was not read [guess: nothing rewrites it]. g5-st1 itself is safe either way: whatever is there is kept as `.sweetorig` and comes back with the restore. | Optional: add `--fail-on 'SWEET DEV: wrote'` to g5-st0b and g5-st-check. Optional: run the g5-st-check command once **before** g5-st1 (label g5-st-pre): `skipped ... (identical)` proves the tool's input is the phone's cache. |
| F7 | L | **`--fail-on 'ramdisk overlaps kernel memory'` (A:761) cannot match.** The shim prints that string to the screen and pstore before the USB log exists (`shim/shim.c:615-617`, the comment at :620 says so); no shim line (`INITRD lo=`) is in any `merged.log` of the four earlier runs [obs], and the runs use `--no-camera`. A RAM disk the bootloader or loader does not accept shows up as `fboot` failing or as "NO DATAGRAM" until the hard timeout, with the phone left in the shim. No kernel runs, so nothing is written. | Reword A:783: "ramp FAIL = fboot error, or no datagram at all until the timeout (then the phone needs the usual recovery to fastboot)". Optional: run the three ramp runs with the camera (drop `--no-camera`) so the shim's line is recorded. |
| F8 | L | **Owner changes during g5-st1.** On the image the cache is `0:80` mode 0644 [obs, item 2]; the overlay writes `0:0` 0644 (one owner per archive; xrun has no `--owner`). The restore renames the original inode back, so `0:80` returns. Whether the cache loader looks at the group is unknown [guess: no, it reads as root]; if it did, the outcome is "launchd starts nothing" -> restore. | None. Mention it in the unknowns. |
| F9 | L | **Throughput (U3) is unknown and the 600 s restart timer is armed before the root mount** (`SWEET RESTART: armed` at merged.log:46, `hfs: mounted` at :155 [obs]), so it runs during the hook. 24 MiB needs more than about 45 KiB/s to finish in time. A reset in the middle is safe (only `PATH.sweettmp` is being written, P:412-445) but leaves a stray 24 MiB temp file. | Read the `N ms` of g5-ramp1 before g5-ramp8/24: stop the ramp if 1 MiB took more than about 10 s. |
| F10 | Info | The cache is already an edited, re-serialised, unsigned-for-its-content file: see item 2. | None. |
| F11 | Info | Leftovers after the series (permanent, `restore` removes nothing it created): `/private/var/dev/ramp-{1m,8m,24m}.bin` (33 MiB), `/usr/local/bin/sweetgl_once`, `/usr/local/bin/sweetgl_selftest`, `/usr/local/lib/libsweetgl.dylib` (21 MB), directories `/usr/local/bin`, `/usr/local/lib`. | Tell the owner of the root. |

## Per-item verdicts

### 1. Archives: PASS [obs]
Parsed with `mkdevoverlay.parse` (the kernel twin); every entry has crc ok and `path_check` = accepted:

| archive (bytes, sha256) | path | mode | uid:gid | size | sha256 of data |
|---|---|---|---|---|---|
| `ov-ramp1.bin` (1048656, `3dfafca0ab5933740b0b4d92bbcaa6fd6816f83a67f2f41b0b0b1070e4e1e1c2`) | `/private/var/dev/ramp-1m.bin` | 0644 | 0:0 | 1048576 | `fbbab289f7f94b25736c58be46a994c441fd02552cc6022352e3d86d2fab7c83` |
| `ov-ramp8.bin` (8388688, `78a4ea7eb532818d0d48766404d013a30a3e233d07212905942caa73e73d0452`) | `/private/var/dev/ramp-8m.bin` | 0644 | 0:0 | 8388608 | `7d212b9c884f5c77896de960ae17cc341cda43b14d6a971f34ca29ebd4badf7f` |
| `ov-ramp24.bin` (25165904, `87c8950075161254e8eb4e88f23f07dd7b8ac373d7c1d5091f589be7c4e2fa02`) | `/private/var/dev/ramp-24m.bin` | 0644 | 0:0 | 25165824 | `206320acc7e9ae70b2e614a2a98ef92a9aa6b3b21cb0aa378c5c16c1a4b0e993` |
| `ov-st1.bin` (21057040, `69ced5a3bf7c0a0cd3d97c51816d18356678125079e83a0d51ddcba9e9a18795`) | `/usr/local/bin/sweetgl_once` | 0755 | 0:0 | 67792 | `cf7a937642870bd8f72e860f5d248cd7f4bef171d9baa60d40c18302463bd616` |
| | `/usr/local/bin/sweetgl_selftest` | 0755 | 0:0 | 68544 | `67733d40c5fee7ed531861fc85880f5f1a930da0180d803261446475e8e3caab` |
| | `/usr/local/lib/libsweetgl.dylib` | 0755 | 0:0 | 20920496 | `f2b31a68a0021178d48e0debfc0704d026e5affb272fa194117e8409ba54d452` |
| `ov-st2.bin` (792800, `011ac014b8ef063b65855b5da96920abb92587ccafd2646f16a04602d3adc904`) | `/System/Library/xpc/launchd.plist` | 0644 | 0:0 | 792710 | `7c764fd52774ca0dd3f471ceee565bc812a3f8069c03d023c857dde71d5b24a4` |
| `ov-st2orig.bin` (792544, `60fe4a3454ae976e1cb9fbf8dfa39fa2a4013f852087e6c33dd479bb938ec484`) | `/System/Library/xpc/launchd.plist` | 0644 | 0:0 | 792460 | `7aab4ecbdaec394205a955f0dce2abf9dd571614b3aee53eaa7439d455763ede` |

* Source dirs: exactly the files above, regular, no symlinks, no extra entries (`find -printf '%y %m %s %p'`). The ramp files are `bytes(range(256))` repeated (compared in full).
* Allowlist (P:125-129): `/private/var/dev/` and `/usr/local/` are `K_DIR` prefixes, `/System/Library/xpc/launchd.plist` is the `K_EXACT` entry. No path uses `/usr/lib/` or the LaunchDaemons prefix.
* Cap: the hook itself has no size cap; the 40 MiB cap is the packer's (`MAX_TOTAL`, mkdevoverlay.py:17, 126). Largest archive 25165904 bytes (24.0 MiB): under it. The boot image is always padded to 64 MiB (`image.meta.json` size 67108864 [obs]); kernel 15.9 MB + 24 MiB + shim fits, and `avbtool` would fail on the host if it did not [src shim/build.sh:420].
* The three binaries in `ov-st1` are byte-identical to `$A/gpu/out/{sweetgl_selftest,sweetgl_once,libsweetgl.dylib}` (same sha256, table above vs `sha256sum $A/gpu/out/*`).
* Re-pack: all six `python3 -I tools/mkdevoverlay.py DIR /tmp/claude-1000/<name>.bin` reproduce the table's sha256 and `cmp` equal to the stored `.bin`.

### 2. The replacement `launchd.plist`: PASS [obs unless tagged]
* **The base is the image's cache.** Read-only catalog walk of `$A/ios-root-oriole-v2.img` (own script, plain `open(..., "rb")`; `7z -thfs` gave "Headers Error" and was not used): `/System/Library/xpc/launchd.plist` = file, mode 100644, owner 0:80, 792460 bytes, 5 extents, sha256 `7aab4ecb...3ede` = `root-ro/launchd.plist` (`cmp` equal). The directory also holds `launchd.plist.sig`, `extensions.plist`, `extensions.plist.sig`. `/usr/local`, `/private/var/dev`, `/usr/lib/libsweetgl.dylib` are absent on the image; no name contains `sweettmp`/`sweetorig`.
* **Only difference = the one job.** 523 -> 524 `LaunchDaemons`; added key `/System/Library/LaunchDaemons/org.pixelios.sweetgl-selftest.plist` = `{Label: org.pixelios.sweetgl-selftest, ProgramArguments: [/usr/local/bin/sweetgl_once, /usr/local/bin/sweetgl_selftest, /usr/local/lib/libsweetgl.dylib], RunAtLoad: true}`; removed none, changed none; `AppExtensions` (421), `AppRemovalServices` (16), `Symlinks` (1), `SystemLibraryTreeState` (1998), `VersionNumber` (7) equal; new minus the job == original. `org.pixelios` does not occur in the original bytes. Re-running `mk_launchd_job.py` gives the same bytes as `ov-st2` (`7c764fd5...`).
* **Re-serialisation risk: none found, because the original is already this serialiser's output.** `plistlib.dumps(plistlib.loads(orig), FMT_BINARY)` is **byte-identical** to the original (792460 bytes, same sha256), with `sort_keys` True and False (Python 3.13.15). So a round trip without the added job changes nothing: key order (every dict already sorted: 0 unsorted of 9887), integer widths, object table, trailer are those the root already boots with. The image builder wrote it the same way (`sweet-root-tweaks.py` `edit_plist` -> `plistlib.dumps`, lines 343-348, 373), and the content shows that tool's edits: 7 `_PanicOnCrasX`, 0 `_PanicOnCrash`, no CommCenter/ReportCrash/spindump job, backboardd `CA_BACKDROP_SCALE=1`.
* Byte-level structure, original vs new: offset size 4 / 4, ref size 2 / 2, objects 22143 / 22150, top object 0 / 0, offset table at 703856 / 704078 and ending exactly at the trailer in both, offsets ascending in both, types `array 1106/1107, ascii 10966/10971, dict 9887/9888, int 153/153, real 29/29, true 1/1, false 1/1`, integer widths `{1: 27, 2: 101, 4: 23, 8: 2}` in both; no data, date, UID, UTF-16, set or null object in either. The +7 objects are the job dict, its array and five new strings (`RunAtLoad`, `Label`, `ProgramArguments` and `true` are reused). Ref size stays 2 (22150 < 65536).
* `SystemLibraryTreeState` is a dict of 1998 framework paths -> `_serviceBundles` (XPC service descriptions), no hash of the tree and no hash of the cache; unchanged. `VersionNumber` 7 unchanged.
* **Does this launchd validate the cache?** Not on this port: four oriole runs (three with the run kernel's stack, incl. `gpu-o5-base-63384af` = kernel `f3e3d113cd63`) log `Code: /System/Library/xpc/launchd.plist Sig: /System/Library/xpc/launchd.plist.sig`, `Using unsecure cache: /System/Library/xpc/launchd.plist`, `Sending validated cache to launchd`, `Cache sent to launchd successfully` [obs]. The `.sig` cannot match the already edited cache [guess: the tweak tool never touches `.sig`, grep], and the root boots to SBCoverSheet. Settings: `launchd_unsecure_cache=1` is in the base boot-args of every image (`build-xnu-image.sh:37`; present in the st1 and restore boot-args I generated with `XNU_IMAGE_ARGS_ONLY=1`) [obs]; `enforce_xpc_cache_code_signing` = `Enabled false` is on the image at `/private/var/preferences/FeatureFlags/Domain/ExtensionKit.plist` (280 bytes, read from the image) [obs] (that flag concerns extension loading, `sweet-root-tweaks.py:409-415` [src]).
* Not known [guess]: whether launchd accepts a cache entry whose plist path has no file on disk (the project only ever removed or edited jobs), and the group change of F8. Both fail as "launchd does not start the job" or "starts nothing".
* **Worst case: launchd rejects or mis-parses the cache -> no jobs, no GUI** (possibly a launchd-initiated panic). The hook is called from `bsd_init` between `siginit` and `bsd_utaskbootstrap` (P:20-31), i.e. before launchd exists, and `sweetdev=restore` needs only the path from the archive (crc and data are not looked at in restore mode, P:464-467, P:484-498). So g5-st-restore works with a bad cache on disk, and also when the file is absent (interrupted between the two renames: `serr` = ENOENT is allowed in the restore branch). g5-st-restore uses `ov-st2` (any archive naming the path would do), expects `restored /System/Library/xpc/launchd.plist` + `done: 1 written, 0 skipped, 0 refused/failed`; g5-st-check uses `ov-st2orig` and expects `skipped ... 792460 bytes (identical)`: correct against P:495, P:521, P:606.

### 3. Run order: PASS, with F2/F3
* ramp1/8/24: one new file each under `/private/var/dev/` (the directory exists after g5-dev1). st0a: three new files, creates `/usr/local/bin` and `/usr/local/lib` (`/usr/local` exists after g5-dev3a; `mkmin` allows it). st0b: byte compare from disk, no write (if identical; F6). st1: the only replacement. Restore, then check.
* One change to the root per run: yes. One new thing exercised per run: not for st1 (F3).
* After the restore the job is gone: the restore renames the original file over the replacement (P:491), so the 524-job file no longer exists and no `.sweetorig` remains; g5-st-check proves the content. The three files in `/usr/local` are then referenced by nothing: no cache entry, no plist under `/System/Library/LaunchDaemons` (the overlay never wrote one), not in any search path except dyld's fallback for a library that is missing elsewhere [guess]. A normal boot is unchanged [guess for "nothing scans /usr/local"].
* If the job is kept, normal boots do change: F2.

### 4. The launcher: PASS, with F4 [src + obs]
* Bounded: tries = 30 (compile-time in the phone build), one `fork` + one `execv` per try (one call site each in the disassembly), each child reaped with `waitpid` before the next try: no fork bomb. Per try 30 s, then `SIGKILL`. Forwarded output 64 KiB per try, lines cut at 512 bytes, `say()` uses a 256-byte buffer with `vsnprintf`. Host test with ASan/UBSan 8/8.
* At most one GPU run per boot: any result other than "SKIP/SKIP without a renderer line" ends the loop (L:231-246); timeout and start failure return at once. Gap: F4. launchd does not restart it (no `KeepAlive`).
* No environment in the phone build: `knob()` returns the default and the log path is the constant `/dev/null` unless `SWEETGL_ONCE_TEST`. Binary [obs]: imports are `fork execv pipe poll read write open close dup2 kill waitpid nanosleep clock_gettime signal memmem vsnprintf fwrite _exit` + stack guard; **no `getenv`, no `strtol`**; `strings` has no `SWEETGL_ONCE_` name; constants `#0x1e`, `#0x7530`, `open` flags `0x209` mode `0x1a4`, `#0x10000`, `#0x200` match the source; all eight message strings match. So it is the committed source built without the test define (not rebuilt here: the cross-build command is not recorded in the repo).
* Child exit: exit status 0..255, `256 + signal`, exit 0 without a PASS line -> 4, `execv` failure -> child `_exit(127)` -> `done after 1 try, status 127 (no result line)`. Correct.
* Harm as root: it opens `/dev/null` (`O_CREAT` is a no-op on devfs), forks, execs `argv[1]` with `argv[2]`, reads a pipe, writes to the sink. No file writes, no IOKit, no network. Mach-O arm64, ios minos 16.0, links only libSystem, ad-hoc signature present (`LC_CODE_SIGNATURE`).
* Small things, no fix needed: after end-of-file on the pipe `waitpid` has no timeout (a child that closed both fds and kept running would block the launcher; this self-test does not); `SIGKILL` reaches only the direct child (it starts no others); the child's stdout is a pipe, so a crash loses its buffered stdout lines (stderr lines survive).

### 5. What the self-test does on the GPU: PASS as "worst case is a hang or a panic" [src]
* It `dlopen`s the dylib, opens `SweetMali` (`IOServiceOpen`), allocates BOs (selector 1), maps them, submits chains built by Mesa on the phone (selector 3), waits (selector 4), reads back.
* The boundary is the kernel: the user client is root-only (`clientHasPrivilege(... Administrator)`), single (`OSCompareAndSwap(0, 1, &sweet_mali_client_open)`), needs `sweet_mali_ready()`. A BO is zeroed kernel memory the kernel allocated itself; the physical pages put into the GPU's one address space come from `getPhysicalSegment` of that descriptor, never from userland; sizes are capped (256 MiB per BO, 1 GiB wired, 4096 BOs). `submit` requires the chain head inside a BO of this client (`owns()`), 8-byte aligned and inside the VA window (0104 `sweet_mali_submit`). Everything a chain points to is a GPU VA: it resolves to this client's BOs or faults in the GPU MMU, which 0104 answers with a soft reset. So a root process with arbitrary chains can corrupt its own buffers, hang or fault the GPU, exhaust up to 1 GiB of wired memory, or trip a kernel bug (panic). It cannot make the GPU read or write other physical memory, and has no path to the UFS.
* A panic or hang ends the boot like every run ends (reset, journal replay). With the replaced cache on disk the next run is g5-st-restore.
* Proven on hardware: the core's map/submit/wait with recorded chains (G2 `0x100`, G4). Not proven on oriole: the IOKit layer (open, `boNew`, `IOConnectMapMemory64`, close/teardown) and Mesa's on-phone chains (F3).
* One client: `sweetmali=1` runs no kernel self-test (bit 1 clear) and no replay (bit 8 clear); without `sweetenv` nothing injects libsweetgl into backboardd, and the dylib's EAGL shim installs only in a process named backboardd (research 52 s.11.5 c). The self-test is the only client [src].

### 6. U1 / U4: PASS, with F7/F9
* The ramp (1, 8, 24 MiB of data; archives 1.0, 8.4, 25.2 MB) comes before `ov-st1` (21.06 MB) and the 24 MiB step is larger than it. Each ramp file is written, so the ramp also measures U3.
* Too large for the boot image: fails on the host (`avbtool`, 64 MiB). Too large for the bootloader or misplaced: either `fastboot boot` fails (phone stays in fastboot), or the shim finds `rd_end > phys_base && rd_start < phys_base + mem_size` and returns `ramdisk overlaps kernel memory` without copying anything (loader.c:95-96), or the bootloader hangs. In every case XNU does not run and nothing is written. Detection: F7.
* A damaged or truncated RAM disk that still boots: structure walk fails -> `bad archive` (nothing done), or the file's crc32 fails -> `refused ... (crc mismatch)` (P:467); both are in the fail-on list. The ramp content is a fixed pattern under a crc.
* AMFI (U4): `sweet_amfi_vnode_check_signature` sets `CS_SIGNED` and, for a vnode on the root mount, `CS_PLATFORM_BINARY` (built tree `sweet_amfi.c:260-269`); the header comment at lines 12-20 is as the section quotes. Page hashes stay with the kernel. Possible outcomes for our three files: exec refused -> `sweetgl_once` never prints (job not started) ; self-test exec refused -> `done after 1 try, status 127 (no result line)`; killed by code signing -> `status 265`; `dlopen` refused -> forwarded `sweetgl_selftest: dlopen ...` lines, 30 x SKIP, `gave up` (F4). Each is a FAIL line or a missing line, none writes anything.

### 7. Pass lines, fail-on, recovery: PASS, with F1/F6
* `wrote` is printed only after the final rename (P:541); every expect carries the exact size and the full `done:` counts. st0a requires `3 written` plus the dylib's `20920496 bytes`; st0b requires `0 written, 3 skipped`, i.e. size and bytes of all three read back from the disk after a reboot (P:520). st1 requires `(original kept)`: a missing original (lost earlier state) would print without it and FAIL.
* A self-test that did not run cannot PASS: the PASS line exists only as the child's last line forwarded by the launcher through the `/dev/null` tee (0048: process name `sweetgl_once`, 12 characters, in `sweetnull=`; the list is 52 of 127 characters). `sweetgl_selftest` in the list is harmless and unused (its output goes to the pipe).
* The regexes match the printf formats (P:495, 521, 541, 606; L:224-248). `'sweetgl selftest: clear=FAIL|tri=FAIL'` matches either FAIL.
* `hfs_mountroot failed` / `cannot mount root` is scored FAIL by xrun_report.py:49, 425 by itself; "stop at the first FAIL" (A:782, A:679) is stricter than plan stop H5 (`.hermes/plans/2026-11-14_oriole-oneshot-2.md:73`: two consecutive GUI runs).
* Recovery text: `tools/fflash-userdata.sh $A/ios-root-oriole-v2.img --yes-erase-userdata --force`, by the user, is correct: the tool refuses an already recorded image without `--force` (fflash-userdata.sh:16) and v2 (`a09f45bb...`, 2026-11-12T14:36:01Z) is the last line of `flashed-userdata.txt` [obs].
* Add to the outcomes list: g5-st-restore printing `restore ...: no .sweetorig` (in the fail-on list) means g5-st1's change never reached the disk; g5-st-check then shows `skipped ... (identical)` and the root is as before.

### 8. Other
* F2, F5, F8, F9, F11 above.
* `mk_launchd_job.py`: input pinned by sha256, refuses an existing key or label, checks its own output reads back as input + job before writing, writes via temp + `os.replace`. Test 6/6 including the real cache (not skipped on this host).
* The kernel for these runs (`f3e3d113cd63...`, 15915840 bytes) is the one the dev-overlay review examined; not re-examined here.

## Proposed g5-st1 command (F1; not run)

```bash
SWEET_USBLOG_PORT=64738 tools/xrun "${C[@]}" "${R[@]}" --overlay $A/gpu/g5/ov-st2 --gpu-handover --intent gpu --label g5-st1 \
  --args 'sweetmali=1 sweetnull=backboardd,SpringBoard,sweetgl_once,sweetgl_selftest' \
  --expect 'SWEET DEV: wrote /System/Library/xpc/launchd.plist 792710 bytes \(original kept\)' --expect "$D 1 written, 0 skipped, 0 refused/failed" \
  --expect 'sweetgl selftest: clear=PASS tri=PASS' --stop-on SBCoverSheet --stop-on 'sweetgl_once: (done|gave up)' \
  --fail-on 'sweetgl_once: gave up' --fail-on 'sweetgl selftest: clear=FAIL|tri=FAIL'
```

## Commands run, with results [obs]

| command (host tools through `pixos-run.sh`) | result |
|---|---|
| `python3 -B tools/test_sweetgl_once.py` | `Ran 8 tests in 1.241s` / `OK` |
| `python3 -B tools/test_mk_launchd_job.py` | `Ran 6 tests in 1.149s` / `OK` |
| `python3 -B tools/test_mkdevoverlay.py` | `Ran 20 tests in 0.459s` / `OK` |
| `python3 -B tools/test_build_overlay_args.py` | `Ran 8 tests in 0.890s` / `OK` |
| `python3 -B tools/test_devoverlay_kernel.py` | `Ran 4 tests in 10.396s` / `OK` |
| `python3 -I tools/mkdevoverlay.py $A/gpu/g5/<d> /tmp/claude-1000/<d>.bin` for the six dirs, `sha256sum`, `cmp` with the stored `.bin` | `1 files, 1048656 bytes` `3dfafca0...`; `1 files, 8388688 bytes` `78a4ea7e...`; `1 files, 25165904 bytes` `87c89500...`; `3 files, 21057040 bytes` `69ced5a3...`; `1 files, 792800 bytes` `011ac014...`; `1 files, 792544 bytes` `60fe4a34...`; all six equal to the table and to the stored archive |
| `sha256sum $A/gpu/out/* $A/gpu/g5/ov-st1/usr/local/{bin,lib}/*` | pairs equal (`cf7a9376...`, `67733d40...`, `f2b31a68...`) |
| `python3 -I tools/mk_launchd_job.py root-ro/launchd.plist OUT --sha256 7aab4ecb...` | `jobs 523 -> 524, 792710 bytes, sha256 7c764fd5...24a4`; `cmp` equal to `ov-st2/.../launchd.plist` |
| read-only HFS catalog lookup on `ios-root-oriole-v2.img` (own script) | `volume HX block 4096 catalog node 8192 leaf nodes 16863`; `launchd.plist file mode 100644 0:80 size 792460 extents 5 sha256 7aab4ecb...3ede`; `/usr/local ABSENT`; `/private/var/dev ABSENT`; `ExtensionKit.plist ... size 280`; image size and mtime unchanged afterwards |
| `file`, `strings`, `llvm-nm -u`, `llvm-objdump --macho --private-headers`, `llvm-objdump -d` of `$A/gpu/out/sweetgl_once` | item 4 |
| `XNU_IMAGE_ARGS_ONLY=1 tools/build-xnu-image.sh ...` for st1 and restore (test hook: no build, no phone) | st1 520 bytes: `... launchd_unsecure_cache=1 ufsrw=1 rootrw=1 ... sweetgpu=0x1C500000 sweetmali=1 sweetnull=backboardd,SpringBoard,sweetgl_once,sweetgl_selftest sweetremote=1 sweetusblogport=64738 sweetdev=1 ...`; restore 431 bytes with exactly one `sweetdev=restore` |
| `python3 -I tools/run-preflight --intent-check ...` for the labels g5-ramp1, g5-st0a, g5-st1 (intent gpu), g5-st-restore, g5-st-check | rc 0 each |
| `grep` in `logs/runs/*/merged.log` | `Using unsecure cache` in 4 runs; no `SWEET MALI: uc ` line in any; `SWEET RESTART: armed` at :46, `hfs: mounted` at :155, launchd at :160, SBCoverSheet +139 s (base run) |

Plist comparison script (`python3 -I plcmp.py ORIG NEW`), shortened to the parts whose output is quoted:

```python
import sys, struct, hashlib, plistlib, collections
KEY = "/System/Library/LaunchDaemons/org.pixelios.sweetgl-selftest.plist"
def structure(raw):
    osz, rsz, nobj, top, toff = struct.unpack(">6xBBQQQ", raw[-32:])
    offs = [int.from_bytes(raw[toff + i * osz: toff + (i + 1) * osz], "big") for i in range(nobj)]
    names = {0: "simple", 1: "int", 2: "real", 3: "date", 4: "data", 5: "ascii", 6: "utf16", 8: "uid", 10: "array", 12: "set", 13: "dict"}
    hist, intw = collections.Counter(), collections.Counter()
    for o in offs:
        m = raw[o]; t = names.get(m >> 4, "?")
        if t == "simple": t = {0: "null", 8: "false", 9: "true"}.get(m & 15, "?")
        if t == "int": intw[1 << (m & 15)] += 1
        hist[t] += 1
    return dict(size=len(raw), offset_size=osz, ref_size=rsz, num_objects=nobj, top_object=top, table_offset=toff,
                table_end_is_trailer=(toff + nobj * osz == len(raw) - 32), offsets_ascending=(offs == sorted(offs)),
                types=dict(sorted(hist.items())), int_widths=dict(sorted(intw.items())))
A, B = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
a, b = plistlib.loads(A), plistlib.loads(B)
for k in sorted(set(a) | set(b)):
    if k != "LaunchDaemons": print("section", k, "equal", a.get(k) == b.get(k))
ja, jb = a["LaunchDaemons"], b["LaunchDaemons"]
print("jobs", len(ja), "->", len(jb), "added", sorted(set(jb) - set(ja)), "removed", sorted(set(ja) - set(jb)), "changed", [k for k in ja if k in jb and ja[k] != jb[k]])
bb = plistlib.loads(B); del bb["LaunchDaemons"][KEY]; print("new minus job == orig:", bb == a)
sa, sb = structure(A), structure(B)
for k in sa: print(k, "orig", sa[k], "new", sb[k])
for sk in (True, False):
    print("round trip of orig byte-identical, sort_keys", sk, plistlib.dumps(plistlib.loads(A), fmt=plistlib.FMT_BINARY, sort_keys=sk) == A)
```

Output (full script; unrelated lines left out):

```
orig sha256 7aab4ecbdaec394205a955f0dce2abf9dd571614b3aee53eaa7439d455763ede 792460
new  sha256 7c764fd52774ca0dd3f471ceee565bc812a3f8069c03d023c857dde71d5b24a4 792710
  section AppExtensions            equal=True
  section AppRemovalServices       equal=True
  section Symlinks                 equal=True
  section SystemLibraryTreeState   equal=True
  section VersionNumber            equal=True
jobs 523 -> 524 added ['/System/Library/LaunchDaemons/org.pixelios.sweetgl-selftest.plist'] removed [] changed []
new job: {'Label': 'org.pixelios.sweetgl-selftest', 'ProgramArguments': ['/usr/local/bin/sweetgl_once', '/usr/local/bin/sweetgl_selftest', '/usr/local/lib/libsweetgl.dylib'], 'RunAtLoad': True}
new minus job == orig: True
org.pixelios anywhere in orig bytes: False | labels with pixelios in orig: []
python types orig: {'dict': 9887, 'str': 12154, 'bool': 4752, 'list': 2969, 'int': 832, 'float': 121}
python types new : {'dict': 9888, 'str': 12158, 'bool': 4753, 'list': 2970, 'int': 832, 'float': 121}
  offset_size            orig=4      new =4
  ref_size               orig=2      new =2
  num_objects            orig=22143  new =22150
  top_object             orig=0      new =0
  table_offset           orig=703856 new =704078
  table_end_is_trailer   orig=True   new =True
  offsets_ascending      orig=True   new =True
  types                  orig={'array': 1106, 'ascii': 10966, 'dict': 9887, 'false': 1, 'int': 153, 'real': 29, 'true': 1}
                         new ={'array': 1107, 'ascii': 10971, 'dict': 9888, 'false': 1, 'int': 153, 'real': 29, 'true': 1}
  int_widths             orig={1: 27, 2: 101, 4: 23, 8: 2}   new ={1: 27, 2: 101, 4: 23, 8: 2}
round trip of orig (sort_keys=True) byte-identical: True (len 792460, sha256 7aab4ecbdaec3942)
round trip of orig (sort_keys=False) byte-identical: True (len 792460, sha256 7aab4ecbdaec3942)
dicts in orig whose stored key order is not sorted: 0 []
first differing byte offset: 28
jobs with RunAtLoad: 115 UserName: 399 ProgramArguments: 395 Program: 157 MachServices: 478 POSIXSpawnType: 290 _PanicOnCrash: 0 _PanicOnCrasX: 7
backboardd env: {'CA_BACKDROP_SCALE': '1'}
dropped jobs present: []
```

Temporary files were written only under `/tmp/claude-1000/` and removed; `build-xnu-image.sh`'s own temp files are removed by its trap.

## Not verified

* Anything on the phone: that the live `/System/Library/xpc/launchd.plist` is still the image's (F6), the state left by the dev runs, free space (4.3 GiB free on the image).
* launchd's behaviour for an added job with no plist file on disk, for the `0:0` group (F8), and for exec from `/usr/local/bin`; launchd is closed source and was not disassembled. The evidence is only that it already runs an edited, plistlib-written cache through the "unsecure cache" path.
* AMFI / dyld on files written by the overlay (U4), and the dyld fallback path claim in item 3: source reading only.
* That `$A/gpu/out/sweetgl_once` is a build of exactly commit 7d3a360: strings, imports and constants match; not rebuilt (no recorded cross-build command).
* The Mesa side beyond the self-test's `main` and `sweetgl_init`: whether Panfrost screen/context creation submits a job (F4), and what chains it builds.
* The IOKit user-client paths on hardware (F3); 0104/0105 were read for the checks named in item 5, not re-reviewed line by line.
* The shim's path after `BOOT XNU failed` (what the phone does until the user or the timeout resets it) and the bootloader's behaviour with a 41 MiB ramdisk (U1): that is what the ramp is for.
* The proposed commands were not executed; only their boot-args and the preflight were generated.
