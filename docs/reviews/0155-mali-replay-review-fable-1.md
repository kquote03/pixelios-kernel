VERDICT: SAFE

# Review of xnu patch 0155 `sweet-mali-replay` (`sweetmali=0x100`), reviewer fable 1

Range `git diff c5f8c64..5083d32` (branch `wip/oriole-gpu`), reviewed 2026-11-15/16. I did not write the patch, did not change it and ran
nothing on the phone. No finding has to be fixed before the first run; F1-F3 should be fixed before the replay is reused by anything else.

Tags: **[obs]** = I ran it and saw the result, **[src]** = read from source, **[guess]** = not verified.

Citations: `P:n` = line n of `xnu/patches/0155-sweet-mali-replay.patch` at 5083d32. `H:n` = line n of the new file
`bsd/dev/sweet_mali_replay.h` (patch line = n + 322). `C:n` = `bsd/dev/sweet_mali.c` as patched through 0155 (1008 lines; the tree in
`~/pixelios-artifacts/localbuild/work/gpu/xnu` was re-patched with 0157 while I worked, so its current numbers are about 109 higher; the
function name is given each time).

## Findings

| id | sev | finding | required fix |
|---|---|---|---|
| F1 | L | `sm_replay_job` treats every `sweet_mali_submit` failure as "no job running" (P:179-183: `goto out` with `safe` still 1, then unmap + `kmem_free` at P:227-231). `ETIMEDOUT` from submit means `sm.busy` stayed set for 6 s, i.e. a job is in flight (`sweet_mali_submit`, C:443-447); `EBUSY` means the slot is not clean (C:456-460). Not reachable in this patch: the replay is the only submitter and every earlier chain was waited to completion or ended the replay [src]. | Not needed for the first run. Before any second submitter can coexist (IRQ patch, user client): set `safe = 0` for a submit result other than `EINVAL`. |
| F2 | L | BO `label` is printed with `%s` (P:163) but the parser only forces a NUL at byte 23 (H:170-173, `r[63] != 0` at H:261); control characters pass. My input H7 (label `x\nSWEET MALI: replay...`) is accepted [obs]. A crafted capture can put a fake `SWEET MALI: replay tri=PASS` line in the log that `xrun --expect` matches. Only printed on a map refusal. Test-result integrity, not phone safety. | Restrict `label` to printable ASCII in `smr_job_parse` and `check_malijob`, or do not print it. |
| F3 | L | The 16 KiB page size is assumed, not checked: `smr_pa` has `SMR_MAX_BO_BYTES / 0x4000` = 2048 slots (P:39-41) and is filled for `n = b.size / PAGE_SIZE` pages (P:147, P:153-156); `PAGE_SIZE` is the runtime `vm_page_size` (`osfmk/mach/arm/vm_param.h:91`). With 4 KiB pages n could reach 8192 and write past `smr_pa`. The existing `sweet_mali_map` has the same assumption (`pa16k[i] + 0x1000 * k`, k < 4) and this port uses 16 KiB pages (`tools/mkpayload.py` `PAGE = 0x4000`) [src]. | One line in `sm_replay`: refuse unless `PAGE_SIZE == SMR_BO_PAGE` (or refuse a BO with `n > SMR_PA_MAX`). |
| F4 | L | `tri` in the real container has `crc_valid = 0` [obs, `pack_gpujob.parse` of `gpujob-clear-tri.bin`]: its PASS rests on three pixels only (`smr_rt_check`, H:334-353). | After the first hardware run, record the CRC in the capture. |
| F5 | L | On a failed chain the replay prints the job header at the chain head `jc[c]` (P:195-197), not the job that failed; `JS_HEAD`/`JS_TAIL` are not printed. Diagnostic only; `JS_STATUS` and, for an MMU fault, `AS0_FAULTSTATUS`/`FAULTADDR` are printed by the core. | Optional: print `JS_HEAD`/`JS_TAIL` in `check_job` (read-only). |
| F6 | L | The `GPUJOB` physical address is not range-checked before `ml_static_ptovirt` (P:272-276); a PA outside kernel RAM panics in `phystokv` (`osfmk/arm64/arm_vm_init.c:379`). Same code as the WLANFW precedent (`sweet_wlan.c` `sweet_wlan_fwcheck`); the property is written by `mkpayload.py` and lies below `topOfKernelData` (`shim/loader.c:107-120`). Worst case a panic. | None. |
| F7 | L | The kernel accepts two container entries on the same bytes and duplicate job names (my input H8a [obs]); only `pack_gpujob.pack` refuses duplicate names. Harmless (the container is read-only). | None. |
| F8 | info | `tools/pack_wlanfw.pack([])` now raises `ValueError`; before it returned a 16-byte count-0 container that `parse` refused [obs, differential below]. No other behaviour change found. | None. |

## Per-requirement verdicts

**1. Bounds and overflow: PASS** [src + obs].
- Container (`smr_container_parse`, H:114-151): length 16..16 MiB (H:119), magic (H:122-126), version, count 1..4 and table inside the
  buffer (H:128; `16 + 16 * n` is at most 80 because n is bounded first in the same `||` chain), tag (H:134), length non-zero, offset
  8-aligned, after the table, `offset <= len`, `length <= len - offset` (H:140-141, subtraction form, no wrap), crc32 over a range already
  proven inside (H:144).
- `.malijob` (`smr_job_parse`, H:194-329): `len >= 64`, `<= 16 MiB`, `file_size == len` (H:201); version, header size, flags, chains 1..8,
  BOs 1..64 (H:219-221); GPU_ID (H:223); every table through `smr_in_file` in 64-bit on 32-bit inputs (H:154-157, H:226-227); name charset
  and termination (H:231-251, `name[9]`); per BO size 16 KiB..32 MiB and aligned, VA aligned and inside the window, `size <= END - va`,
  `data_len <= size`, flags, label NUL, `data_off` inside the data area and the file, `data_len <= len - data_off` (H:260-262); sum of BO
  sizes <= 64 MiB (H:265-268); overlap test on values < 2^33 (H:272); chains 128-aligned with 64 bytes inside one BO (H:284); RT format,
  dimensions <= 4096, `stride >= width * 4` in 64-bit, `stride * height` in 64-bit (max 2^44) inside one BO (H:305-312); pixels inside
  the RT (H:323).
- Kernel uses: `memcpy(kva, buf + data_off, data_len)` into `size` bytes (P:148-152); RT rows end at `(h - 1) * stride + w * 4 <= stride * h`
  (H:340-341); pixel reads `y * stride + x * 4 + 4 <= stride * h` (P:214-216, H:344); fault header read of 32 bytes at a `jc` that has 64 (P:195).
- My own inputs (below) found no accepted file that breaks an invariant.

**2. Fixed VAs only: PASS** [src + obs]. `sm_map_at` takes the VA only from `mali_va_reserve` (P:69), which refuses a misaligned VA or
page count, a VA outside `MALI_VA_START..MALI_VA_END`, a range past the end (H:361-364) and any taken page before it sets a bit
(H:366-370). The parser already refused overlaps (H:272). A refusal returns before any page-table write (P:70-73); the caller frees the
BO (P:166-169). A failed `mali_pt_map` removes what it wrote and releases the VA (P:80-88). In my consumer model every accepted job's BOs
reserve with result 0 on an empty allocator (271,786 jobs) [obs].

**3. No new register writes: PASS** [src + obs]. The patch adds no `wr(` call (`grep -c '^+.*\bwr('` = 0 [obs]) and no `ml_io_map`.
`sm_map_at` (P:52-98) is `sweet_mali_map` (C:341-393) line for line except: the VA comes from `mali_va_reserve` instead of
`mali_va_alloc`, a misaligned PA returns `EFAULT` instead of `EINVAL` (both: nothing mapped), and there is no `*va_out`. Registers
reached: `AS0_LOCKADDR`/`AS0_COMMAND` (`as_flush`), `JS*_HEAD_NEXT/AFFINITY_NEXT/CONFIG_NEXT/COMMAND_NEXT` (`sweet_mali_submit`), and on
a fault the completion thread's `JOB_IRQ_CLEAR`, `MMU_IRQ_CLEAR`, `JS_COMMAND` hard stop and `sm_reset` — all existing 0104 code that
the self-test also runs through. Bring-up with `0x100` alone is the `sweetmali=1` bring-up (`sm_bringup`). No clock, voltage, PMIC,
GIC or IRQ-mask code is added. Note: the replay runs real vertex/tiler/fragment jobs, a heavier GPU workload than the WRITE_VALUE
self-test; the CPU-side register set is the same.

**4. Bounded waits: PASS** [src].
- Per chain: `sweet_mali_submit` up to 6 s (C:445, unreachable here), `sweet_mali_wait` 2 s (P:184), drain 4 s (P:187). The core
  hard-stops at 3 s, resets 0.5 s later; `sm_reset_do` holds the lock for at most about 1.5 s (2 x 500 ms reset polls, 2 x ~100 ms
  power polls in `sg_power`, 2 x 100 ms `as_idle`), so the drain returns by about 4.9 s after the job start, inside the 6 s. If not,
  `safe = 0` and everything is leaked (P:188-190).
- Map/unmap: `as_flush` is 3 x 100 ms at most and a failure marks the device dead, so at most one slow flush per boot.
- Only the last chain run can time out (a failure ends the replay). Format maximum (4 jobs x 8 chains): 31 x 2 s + 6 s + 1.5 s, about
  70 s, plus the bit-wise crc32 of at most 16 MiB container and the RT [guess: about 1 s]. Round figure: **80 s worst case**. For the real
  container (1 + 2 chains): at most 2 x 2 s + 7.5 s, about **12 s**; a single failing chain costs at most 7.5 s.

**5. Fault handling: PASS** [src]. The core prints `MMU fault ... AS0_FAULTSTATUS ... FAULTADDR` and `job ... failed JS_STATUS`
(`check_job`, C:253, C:267), resets through `finish` -> `sm_reset` (C:232-238). The replay adds the header words (P:195-197), leaves
the chain loop (`goto out`), and sets `*stop` because `pass` is 0 (P:237); `sm_replay` then runs no further job (P:281). After a failed
reset `sweet_mali_wait` returns `EIO` (not `ENXIO`), but `sweet_mali_unmap` then returns `ENXIO`, so the BOs are leaked (P:227-234). See F5.

**6. Gating: PASS** [src + obs]. Without bit 0x100 the init condition is the old one (P:296-298) and the new block is skipped (P:307).
With it: `SweetMali::start` returns false when `(mode & 3) == 0` (`iokit/Kernel/arm/SweetMali.cpp:61`), so no service and no user
client; `0x101`/`0x102` refuse the replay (P:308-309); `0x103` returns early as before. `sweetgpujob=1` together with it makes
`sm_bringup` refuse (cores already on). Payloads built by the old and new `mkpayload.py` without `--gpujob` are byte-identical [obs].

**7. Memory: PASS** [src]. `kmem_alloc(KMA_DATA | KMA_ZERO)` (wired); `kmem_free` only (a) when `sm_map_at` returned an error other
than `EIO`/`ENXIO`, which by contract mapped nothing (P:164-169), or (b) in `out:` while `safe` is still 1 and `sweet_mali_unmap`
returned 0, i.e. page-table entries cleared and AS0 locked + flushed (P:227-231). `safe` is cleared on a map `EIO`/`ENXIO`, a wait
result other than 0/`EIO`, a drain that did not end, and any unmap failure; then every remaining BO is leaked with a message. Wait 0 or
`EIO` both mean `finish()` ran, so no job is in flight. Exception: F1 (unreachable). Copies: see item 1; `smr_pa` see F3. Cache: each
page clean+invalidated after the copy (P:153-156) and again in `sm_map_at` (P:75) before the GPU can read; `smr_cpu` clean+invalidates
every page of the range before the CPU reads the RT or the job header (P:104-110). The CPU does not write the BOs after the copy, so the
clean cannot overwrite GPU data.

**8. Worst case and tools: PASS** [src + obs]. The patch touches no storage, PMIC, fuse or secure-world path; all state is RAM and GPU
registers, gone after a power cycle. Worst outcomes: panic (bad PA, F6), a boot delayed by up to about 80 s, a dead GPU with leaked wired
memory until reboot. Tools: `pack_wlanfw.pack`/`parse` are wrappers over the generic functions; 3000 random containers are byte-identical
between c5f8c64 and 5083d32 and 60,000 corrupted containers get the same accept/refuse result [obs]; `test_pack_wlanfw.py` 5/5 [obs].
`mkpayload.py`: payload without `--gpujob` and payload with `--wlanfw` are byte-identical old vs new (sha256
`027b69af...` and `bdc5ebe5...`) [obs]; with `--gpujob` one piece is added after WLANFW, `kernel_end` grows by 0x4000 and the WLANFW
bytes are present unchanged [obs]. `build-xnu-image.sh`/`xrun` only pass the flag through; `--gpujob` requires `--gpu-handover`.

**9. Hostile job chains: contained, and it does not matter for phone safety** [src; hardware property: guess].
The parser checks where a chain starts, not what it contains. A hostile or broken chain can make the GPU read and write anywhere the
GPU MMU lets it: AS0, whose tables hold exactly this job's BOs (no self-test buffer, no user client; BOs of an earlier job are unmapped
and flushed before they are freed). The page tables themselves are not mapped in AS0, the slot's address space is fixed by
`JS_CONFIG_NEXT` written by the kernel, and a job descriptor has no field that selects an address space or a physical address
[guess, the same isolation kbase relies on between processes; not verified on this chip]. Any access outside the BOs is a translation
fault: hard stop, reset, replay ends. Inside the BOs the chain can corrupt its own descriptors, shaders, heap and RT: fresh private kernel
pages, so only the test result changes. A chain that never ends is stopped at 3 s and reset; if stop and reset both fail the GPU may keep
running on the leaked pages until the power cycle [guess: no thermal consequence; not verified].

## Commands run

All through `pixos-run.sh`; heavy ones under `flock ~/pixelios-artifacts/scratch/heavy.lock`. The worktree HEAD had moved to 340cc27
(patch 0157) when the build ran; `git diff 5083d32..HEAD` over the reviewed patch, 0104, the host test, the mutant list and the tools is
empty except six added lines in `shim/build.sh` for the 0157 host test [obs].

| Command | Result [obs] |
|---|---|
| `cd shim && PIXELIOS_HOST_TESTS=always ./build.sh --board oriole --avb --pstore-fast --usb-gadget --out out/gpu-review` | exit 0; last lines `PASS header v4 (raw): kernel=299008 ramdisk=103; host tests & safety checks passed` / `PASS header v4 (lz4): kernel=51017 ...`. `out/gpu-review/mali_replay_host_test` and `mali_replay_asan_host_test` rerun by hand: `mali_replay_host_test: PASS` twice. Extracted `sweet_mali_replay.h` identical to the patch hunk and to the patched tree. |
| `python3 -B tools/test_mali_replay_mutants.py` | 48 lines `killed`, no survivor, `Ran 2 tests in 11.374s` `OK` |
| `python3 -I tools/test_pack_gpujob.py` | `Ran 7 tests in 0.013s` `OK` |
| `python3 -I tools/test_pack_wlanfw.py` | `Ran 5 tests in 0.001s` `OK` |
| `tools/patch-numbers check` | `RESULT: PASS this tree's patch numbers do not overlap other branches or reservations` |
| `patch -R --dry-run -p1 < 0155...patch` in the patched tree (0155 state) | both files check clean |
| old vs new `pack_wlanfw.py` differential (scratch script) | `differential: 3000 packs byte-identical, 60000 corrupted parses agree`; `pack([])` old = 16-byte container, new = `ValueError` |
| old vs new `mkpayload.py` on `xnu-local-lane-g-63384af/kernel.development.sweet` + generated oriole ADT | `PLAIN_IDENTICAL`, `WLANFW_IDENTICAL`; with `--gpujob`: `wlanfw: ('0x88269c000', '0x2be8')`, `gpujob: ('0x8826a0000', '0x1e40')` |
| `pack_gpujob.parse` of `gpujob-clear-tri.bin` (sha256 `f1f568d7...266fba`, matches note 52 s.8) | `clear`: 1 chain, 8 BOs, 17186816 bytes, crc_valid 1; `tri`: 2 chains, 9 BOs, 18235392 bytes, crc_valid 0 |

### My hostile inputs (scratch copy, ASan + UBSan, not committed; no bug found)

Each accepted file also went through a model of `sm_replay_job` (exact-size heap block per BO, the kernel's `memcpy`, `mali_va_reserve`
per BO, the 32-byte header read, `smr_rt_check`, the three pixel reads). Output: `hostile: done, 271786 jobs went through the consumer`, rc 0.

| id | input | result |
|---|---|---|
| H1 | `stride * height` = 2^32 (0x100000 x 4096), 2^32 + 4096, and stride 0xFFFFFFFF | refused, code 19 |
| H2 | BO in the last 16 KiB below 4 GiB, RT ending exactly at 4 GiB, `jc` 128 bytes below the end; then one row more, RT shifted by 4, `jc` = 4 GiB, BO size past 4 GiB | first three accepted and consumed cleanly; the rest refused (19, 19, 18, 16) |
| H3 | BO sizes summing to exactly 64 MiB / 64 MiB + 16 KiB | accepted / refused 16 |
| H4 | tables aliasing each other: chain table on the BO table, RT block on the chain table, BO table over the data | refused 18 / 19 / 16, no out-of-bounds read |
| H5 | `data_off == len` with `data_len` 0 / 1, `data_off` = 2^63 + len, `data_len` one past the file | accepted / refused 16 x 3 |
| H6 | name of 8 characters without NUL; `a\0b`; `a\n` | accepted with `strlen == 8`; refused 15 x 2 |
| H7 | BO label with a newline and a fake log line | accepted -> F2 |
| H8 | container: two entries on the same bytes; `offset + length` wrapping u32; `offset == len`; 9 bytes in the last 8; a valid 8-byte entry; count 0xFFFFFFFF and 0x10000001; every length 0..47 | first accepted (F7); 8-byte entry passes the container and is refused by the job parser (10); the others refused (5, 5, 5, 3, 3) |
| H9 | 400,000 files with 1-3 words anywhere in a 4-BO job replaced by boundary values (0, page and window edges, 2^32, 2^63, all-ones, `len - k`) | 271,774 accepted, all consumed without a sanitizer report or a failed invariant |

## Not verified

- I did not build the kernel. `~/pixelios-artifacts/xnu-local-lane-g-63384af/kernel.development.sweet` exists (15.9 MB, 2026-11-15 20:42)
  and `scratch/xnu-local-build.log` ends `make exit 0` [obs]; compiler warnings for `sweet_mali.c` were not inspected.
- Nothing ran on hardware: whether the captured chains execute, whether `sm_reset` recovers after a real MMU fault on this chip, and the
  real register values are open.
- The Mali MMU isolation in item 9 and the GPU's behaviour when left running on a dead device are [guess].
- Whether the boot thread that calls `sweet_mali_init` (`sweet_ufs.c` -> `sweet_gpu_gs101_init`) tolerates a block of up to 80 s without
  another timer firing; the self-test already blocks there for up to 4 s [src].
- That `flush_dcache64(pa, PAGE_SIZE, TRUE)` cleans and invalidates on this core is taken from `osfmk/arm/caches.c:61-92` [src] and from
  its existing use by the self-test, not measured.
- Patch 0157 (after the range) changes the same file; its interaction with the replay is outside this review.
