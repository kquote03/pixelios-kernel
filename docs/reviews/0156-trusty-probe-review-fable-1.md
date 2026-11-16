VERDICT: SAFE

# Review: xnu patch 0156 sweet-trusty-probe (`sweettrusty=1`), independent

Reviewer: Claude Fable 5.2 (delegate, worked alone, no phone access, did not write or change the patch). Date 2026-11-16.
Commit reviewed: `47673d8` on `wip/oriole-audio`, in a clean detached checkout (`/tmp/claude-1000/review-0156`, removed afterwards).
Line refs: `0156:N` = line of `xnu/patches/0156-sweet-trusty-probe.patch`; `test:N` = `shim/trusty_host_test.c`; `tru:` = `~/pixelios-artifacts/research-src/trusty` at `2494a3e105ef21fd204c44598e1a960f73400b90`; `K` = `~/pixelios-artifacts/xnu-local-audio-47673d8/kernel.development.sweet` (sha256 `12d87b4e...360ab74`).
Tags: **[obs]** = I ran it or read it in the built binary; **[src]** = read from source at the cited line; **[guess]** = not shown by anything I could open.

Summary: the probe can issue only `0xBC00000A` (r1 = 0xFFFFFFFF once, then indexes 0..len-1, len <= 160) and `0xBC00000B` (r1 = 5, once), at most 162 SMCs, once per boot, only with `sweettrusty=1`. Ids, arguments, order and result handling match the stock driver. The built kernel contains exactly three `smc` instructions in this function with these constants. All findings are severity L; none blocks one `sweettrusty=1` run. F1 should be fixed before any later Trusty step (A5) reuses the wrapper.

## Findings

| id | sev | finding | required fix |
|---|---|---|---|
| F1 | L | The SMC wrapper does not set x4-x7 (0156:65-72: they are only in the clobber list). Stock passes them as 0 (`tru:drivers/trusty/trusty.c:78`, `trusty_smc8(r0, r1, r2, r3, 0, 0, 0, 0)`). In `K` they hold whatever `printf` left there at the first SMC [obs, disassembly below]. They are not Trusty arguments (`SMC_NUM_ARGS 4`, `tru:include/linux/trusty/smcall.h:12`), and x7 is an SMCCC client id that matters only for a caller behind a hypervisor; the shim leaves `HCR_EL2 = RW` only (`shim/start.S:43-44, 109-110`), so I expect them to be ignored [guess: the EL3 monitor source is not available]. Worst effect I can construct: the monitor answers an error and the probe stops with a false negative; no other function can be reached, the id is in x0. | None for this run. Before patch A5 (or at the next change of this file): pass x4-x7 = 0 as in/out registers, as stock does. If this run ends in `stop bad-length r0=0xfffffffb` (SM_ERR_BUSY) or another unexplained error, fix F1 and get a delta review before reading it as "Trusty not reachable" (it would be a new build of the same two calls, not a retry with other arguments). |
| F2 | L | The host test does not pin the two function ids as literals: it compares against `SWT_FC_*` from the header under test (test:25, 38, 77-78, 135-140). My mutants "0xBC00000A -> 0xBC000009" and "0xBC00000B -> 0xBC00000C" survive the committed test. For this run the ids are verified in the binary [obs]. | Can follow the run: add `CHECK(SWT_FC_GET_VERSION_STR == 0xBC00000Au && SWT_FC_API_VERSION == 0xBC00000Bu && SWT_API_WANT == 5u && SWT_UNDEFINED_SMC == 0xFFFFFFFFu)` (or make the fake compare literals), together with the mutant script. |
| F3 | L | Cite text: `sm_err.h:8` (0156:112) is the include guard; `SM_ERR_UNDEFINED_SMC` is at `tru:include/linux/trusty/sm_err.h:12`. Research note 50 has the same `sm_err.h:8-26` (should be 12-26). | Correct the comment at the next change of the file. |
| F4 | L | A Trusty that selects API 0-4 gives the same pass-shaped line `SWEET TRUSTY: api=N version=...` (0156:92, 223-227), and the documented `--expect 'SWEET TRUSTY: (api=|stop )'` matches it. Accepting a lower version equals stock (`tru:trusty.c:782-790`) and is safe, but the plan's pass is `api=5` with the stock string. | Driver: compare the line with `api=5 version='Project: slider, Build: 15644732, ...'` by eye or with a stricter `--expect`; anything else is a recorded result, not a pass. No code change needed. |
| F5 | L | No mutant script exists for this patch. I ran 20 ad hoc mutants (below): the committed test kills all but the two id mutants of F2 and three equivalent ones. | Not needed before the run. Add `tools/test_trusty_mutants.py` (or the project's usual form) with F2's pins before A5 builds on this header. |
| F6 | L | Bare `sweettrusty` (no `=1`) does not run the probe: `PE_parse_boot_argn` stores the string "1" (0x31) for a valueless argument (`pexpert/gen/bootargs.c:163-166` of the built tree), so `on != 1` and the line "not known (only 1), not touched" is printed (0156:86-89). This is the safe direction. | None. Use `sweettrusty=1` exactly, as `docs/AUTOMATION.md` says. |

## Per-item verdicts

### 1. Only the two function ids, only these arguments, stock order and result handling: PASS (F1, F2, F3 are L)

- Ids [src]: `SMC_FC_GET_VERSION_STR = SMC_FASTCALL_NR(SMC_ENTITY_SECURE_MONITOR, 10)` (`tru:smcall.h:119`), `SMC_FC_API_VERSION = SMC_FASTCALL_NR(SMC_ENTITY_SECURE_MONITOR, 11)` (`smcall.h:143`), encoding `fast<<31 | smc64<<30 | entity<<24 | fn` (`smcall.h:20-26`), entity 60. `0x80000000 | 60<<24 | 10 = 0xBC00000A`, `| 11 = 0xBC00000B`. Header: 0156:122-123. `TRUSTY_API_VERSION_CURRENT 5` (`smcall.h:142`) = `SWT_API_WANT` (0156:124).
- Stock argument convention [src]: `trusty_fast_call32(dev, SMC_FC_GET_VERSION_STR, -1, 0, 0)` (`tru:trusty.c:731`); the parameter is `u32 a0` (`trusty.c:83`), so -1 becomes 0xFFFFFFFF and is zero-extended into x1 by `smc(unsigned long ...)` (`trusty.c:72-81`). Ours: `0xFFFFFFFFull` (0156:196). In `K`: `mov w1, #-1` (writes x1 = 0x00000000FFFFFFFF) [obs]. My mutant that passes 64-bit -1 is killed by the committed test (test:77, 138).
- Characters: stock `trusty_fast_call32(dev, SMC_FC_GET_VERSION_STR, i, 0, 0)` for `i < version_str_len` (`trusty.c:738-739`); ours 0156:206-207, index strictly sequential.
- API version: stock `trusty_fast_call32(dev, SMC_FC_API_VERSION, TRUSTY_API_VERSION_CURRENT, 0, 0)` (`trusty.c:777-778`); ours 0156:217.
- r2 = r3 = 0: stock `trusty.c:731, 739, 778`; ours 0156:67-68; in `K` `mov x2, xzr; mov x3, xzr` before each `smc` [obs].
- Order: stock `trusty_init_version` then `trusty_init_api_version` (`trusty.c:1109-1111`); ours string then API (0156:196-217), pinned by test:77-81.
- Truncation of r0 to 32 bits: stock returns `s32` from `smc()`'s `unsigned long` (`trusty.c:83, 94`); ours `(uint32_t)b->smc(...)` (0156:171). The fake monitor puts junk in the high half (test:27); my fake `M_HI_JUNK` does it for all three calls.
- Result handling against stock:
  - length: stock `if (ret <= 0) error` (`trusty.c:732`); ours `(int32_t)v <= 0` plus our own cap `v > 160` (0156:202), and 0xFFFFFFFF reported as `undefined-smc` (0156:199).
  - character: stock `if (ret < 0) error` (`trusty.c:740`); ours 0156:210.
  - API: stock `SM_ERR_UNDEFINED_SMC` -> version 0 and continue, `> 5` -> `-EINVAL` (`trusty.c:779-786`); ours stops on 0xFFFFFFFF (0156:220-221, stricter) and on `> 5` (0156:223).
- `SM_ERR_UNDEFINED_SMC` is `0xFFFFFFFF`, "Unknown SMC (defined by ARM DEN 0028A)" (`tru:sm_err.h:12`): the SMCCC "not supported" value (-1) that a monitor returns in x0 for a function id nobody owns. An EL3 that returns 64-bit -1 gives 0xFFFFFFFF after truncation: `stop undefined-smc`, one SMC (my fake `M_UNK64`) [obs on host].
- One issuing site: every call goes through `swt_call` (0156:164-173), which refuses anything `swt_allowed` (0156:152-162) does not list. The glue has one `smc #0` (0156:70). `K` has exactly three `smc` instructions in `_sweet_trusty_init` (0xfffffe0007546e84, ...6f14, ...6ff8) with w0 = 0xBC00000A/-1, 0xBC00000A/x19 (index), 0xBC00000B/5 [obs].
- Not controlled: x4-x7 (F1).

### 2. Bounds, buffer, log hygiene, no repeat with other arguments: PASS

- SMC count: 1 length + at most 160 characters + 1 API = 162 = `SWT_SMC_MAX` (0156:126-127). Three independent limits: the length check (0156:202), the allowlist (`a1 < len && len <= SWT_VER_MAX`, 0156:156), the counter (0156:167). The loop bound is `r->len`, written once (0156:205-206); the length is never asked again.
- Buffer: `ver[161]` (0156:140); the highest store is `ver[i + 1]` with `i <= 159` (0156:214-215). ASan/UBSan clean for length 160 (test:103-105, my `M_MAX_FF`) and for 4 million fuzzed monitors with guard bytes around the struct [obs].
- Log: a character is stored only if `0x20 <= v < 0x7F` and `v < 0x100`, else `?` (0156:214); the string is NUL-terminated on every path (0156:195, 211, 215). Printed with `%s` (0156:92, 94), so a `%` in the string is inert. Pinned by test:119-124; my fakes return 0x80, 0xFF, 0xC3, 0x141, 0x7F, 0x1F, 0x00, 0x7FFFFFFF.
- No retry: each unexpected answer returns through `swt_stop` (0156:197-224); there is no loop around a call and no second argument set. My mutant "ask the length again after a bad length" is killed (test:95). SM_ERR_BUSY (-5) on the first call: one SMC, `bad-length`, no retry (my `M_BUSY`); stock's fastcall path does not retry either (`trusty.c:83-95`; the busy retry is only in the std-call path, `trusty.c:125-135`).
- Once per boot: `sweet_trusty_init` is called at the end of the success path of `sweet_ufs_init` (0156:34; built tree `bsd/dev/sweet_ufs.c:700, 711`), after `su.ready = true`, and `sweet_ufs_rootdev` calls `sweet_ufs_init` only while `!su.ready` (`sweet_ufs.c:1007`) [src].

### 3. SMC wrapper, calling context, what API_VERSION changes: PASS (F1 is L)

- Registers: x0-x3 in/out, x4-x17 in the clobber list, plus `cc` and `memory` (0156:65-72). Stock: x0-x7 in, x0-x7 out, only x8 and lr saved around the `smc` (`tru:drivers/trusty/trusty-smc-arm64.S:18-35`), the rest left to the C ABI (x0-x17 caller-saved). SMCCC: x4-x17 unpredictable in 1.0 and preserved unless they carry results from 1.1; x18-x30, SP and (stock makes the same assumption) FP/SIMD are preserved by the callee. The clobber list covers the wider rule. Same wrapper shape as `xnu/patches/0063-sweet-smp-psci.patch:36` and `0083-sweet-restart.patch:71-77`. In `K` the values kept across the SMCs are in x19-x25 [obs].
- Conduit: `smc #0` from EL1, `HCR_EL2 = RW` only, so the SMC is not trapped to EL2 (`shim/start.S:43-44, 109-110`; `shim/wlansmc.c:10`) [src]. PSCI already uses it (patch 0063). Whether the monitor routes entity 60 to Trusty for this caller is what the run measures [guess until run]; an unrouted id returns 0xFFFFFFFF.
- Context: thread context of the BSD root-device lookup (`sweet_ufs_rootdev`), interrupts enabled, preemptible, other CPUs may be up. Stock issues the same calls from a probe thread, preemptible, on any CPU, without masking interrupts: `trusty_fast_call32` calls `smc()` directly (`trusty.c:83-95`), the `local_daif_mask()` wrapper is only in the std-call helper (`trusty.c:140-150, 176`). Acceptable.
- Blocking / restart: fastcalls (bit 31, `smcall.h:15, 26`) return directly; the interrupted/restart codes (`SM_ERR_INTERRUPTED`, `FIQ_INTERRUPTED`, `NOP_INTERRUPTED`, `sm_err.h:14, 23, 25`) belong to std-calls and are handled only in `trusty_std_call_inner/helper` (`trusty.c:115-138, 166-`). No restart loop is needed and none is present. If such a code came back anyway it is negative as `int32_t`: the probe stops (0156:202, 210).
- What `SMC_FC_API_VERSION` changes: per the header text it "finds and selects" the version; "this call must be made before any calls that are affected by the api version" (`smcall.h:121-136`) [src]. It sets Trusty's current API version, a variable in secure RAM, to min(5, Trusty's maximum) [guess: Trusty's own `lib/sm` source is not in the pinned set; this is my recollection of it]. The versions govern the std-call protocol (restart-FIQ 1, SMP 2, SMP-NOP 3) and the memory-object format (4, 5) (`smcall.h:137-142`); the probe issues no std-call and shares no memory, so nothing consumes the selection in this boot. 5 is the value stock Linux selects on every boot of this phone (stock dmesg:643, note 50). It is not stored: Trusty is loaded again by the boot chain on every power cycle [guess, standard for this platform; no source here shows a persistent write and a fastcall has no storage path]. A later boot and a stock boot negotiate from scratch.

### 4. Default off: PASS

- No boot-arg, or `sweettrusty=0`: `PE_parse_boot_argn` false or `on == 0` -> return before any print or SMC (0156:83-85). In `K`: `cbz w0` / `cbz w8` to the epilogue [obs].
- Any other value: one log line, return, no SMC (0156:86-89); in `K` `cmp w8, #1; b.ne` to the print [obs]. `sweettrusty=2` (reserved for A5) is refused by this build. Name match is exact (`pexpert/gen/bootargs.c:105-106`). A string value cannot produce the integer 1 (F6).
- The other hunks add one `files` line, one prototype, one call (0156:15, 26, 34). `tools/bootargs-known.txt:507` registers the name.

### 5. Worst case: PASS

- Most likely bad outcome: the monitor does not know the id -> `SWEET TRUSTY: stop undefined-smc r0=0xffffffff after 1 smc`; boot continues. By plan stop H9 the secure-world steps are then skipped.
- Worst outcome: the SMC does not return (the calling CPU stays in EL3/Trusty) or the monitor resets the SoC -> hang or reboot during boot, possibly an XNU watchdog panic from another CPU. A power cycle (Power + Volume Down, or `xrun --restart 600,bootloader`) undoes it completely.
- Nothing persistent: no storage, fuse, RPMB or flash path is reachable from two version fastcalls; no std-call (bit 31 is set in both ids), no shared memory, no FF-A call, no TIPC, no GSA mailbox, no MMIO store (the two new files contain no `ml_io_map`/register access; 0156:53-58 includes only printf/boot-arg headers). The kernel state added is one 180-byte static (0156:79).
- `docs/SAFETY.md` "never load/replace secure-world code": not touched; the calls only read a version and select the stock API version. Plan section 11 rule A-1 lists "the fastcalls ... needed for version" for Trusty: inside the allowlist. H9: "not a documented code -> stop, never retry with different arguments" is implemented (item 2).

### 6. Host test covers the rules: PASS (F2, F5 are L)

- Committed test: happy path with call log and order (test:70-81), lower API (83-85), undefined on first call (87-91), negative/zero/too-long/exact-bound lengths (93-107), error mid-string (109-116), unprintable and wide characters (118-124), API undefined and above 5 (126-132), the allowlist itself including foreign ids `0x3C000003, 0x32000014, 0xBC000003, 0x84000073, 0x82000504` (134-142), bad arguments (144-149). Built plain and with ASan/UBSan (`shim/build.sh:596-601`).
- My hostile fake monitors (scratch copy of the header extracted from the patch; each call is checked inside the fake against: length asked once and first, indexes sequential and below the first returned length <= 160, API once, last, with 5, no other id, at most 162 calls; guard bytes around the result; string printable and terminated). All pass, plain and ASan/UBSan [obs]:

| fake | result |
|---|---|
| length 0x7FFFFFFF | `bad-length`, 1 SMC |
| answers to index calls look like new, larger lengths | length stays 4, 6 SMCs, `????` |
| characters 0x80, 0xFF, 0xC3, 0x141, 0x7F, 0x1F, 0x00, 0x7FFFFFFF | all `?` |
| character 0x80000041 at index 2 | `bad-char`, 4 SMCs, string so far `kk` |
| API 0xFFFFFFFE | `bad-api`, raw 0xFFFFFFFE |
| unknown-SMC echo (x0 = function id) on every call | `bad-length` raw 0xBC00000A, 1 SMC |
| echo only on character calls / only on the API call | `bad-char` after 2 SMCs / `bad-api` raw 0xBC00000B |
| always 1 / always 0 | ok, api 1, `?`, 3 SMCs / `bad-length`, 1 SMC |
| SM_ERR_BUSY (64-bit -5) | `bad-length` raw 0xFFFFFFFB, 1 SMC, no retry |
| 64-bit -1 | `undefined-smc`, 1 SMC |
| length 160, every character 0xFF | ok, 162 SMCs, 160 x `?` |
| junk in the high 32 bits of every answer | ok, `ZZ`, api 5 |
| 2,000,000 monitors with random 64-bit answers | all `bad-length`, no rule violation |
| 2,000,000 monitors with random small answers | ok 712999, bad-length 117726, bad-char 814019, bad-api 355256, no rule violation |

- Ad hoc mutants of the header (20), committed test: 15 killed. Survivors: the two id mutants (F2); "counter cap dropped" (equivalent: the other two bounds give the same 162); "no `(uint32_t)` cast" (equivalent: `*out` is `uint32_t`); "undefined on API tolerated in the `> 5` check" (equivalent: the earlier check catches it). Dropping either single length bound is killed (the test expects `bad-length`, the allowlist turns it into `bad-arg` without an out-of-range call).
- Mutant script: not needed before the run; it should follow with F2's literal pins (F5).

## Commands and results

| command | result |
|---|---|
| `git worktree add --detach /tmp/claude-1000/review-0156 47673d8` | clean checkout [obs] |
| `cd shim && PIXELIOS_HOST_TESTS=always ./build.sh --board oriole --avb --pstore-fast --usb-gadget --out out/rev0156` (pixos-run.sh, `flock heavy.lock`) | exit 0, no line starting with `FAIL` in 1499 lines of output; the lock was held by other builds for about 20 minutes first. Trusty lines (log 206-211): `PASS trusty_host_test (compile)`, `PASS trusty_host_test` (the test), `PASS trusty_host_test` (build.sh check), `PASS trusty_asan_host_test (compile)`, `PASS trusty_host_test` (the ASan binary), `PASS trusty_asan_host_test`. Images written to `out/rev0156/` of the temporary checkout (not used) [obs] |
| the four trusty steps of `shim/build.sh:597-601` run by hand in the clean checkout (pixos-run.sh, no lock: two small gcc runs) | header extracted (128 lines); `PASS trusty_host_test` (plain, rc 0); `PASS trusty_host_test` (ASan/UBSan, rc 0) [obs] |
| `tools/patch-numbers check` (pixos-run.sh) | `RESULT: PASS this tree's patch numbers do not overlap other branches or reservations`, exit 0 [obs] |
| `grep -a 'SWEET TRUSTY' K` | 5 strings: `sweettrusty=%u not known (only 1), not touched`, `probe starts (2 version fastcalls only)`, `api=%u version='%s'`, `stop %s r0=0x%x after %u smc (version so far '%s')`, `probe done smc=%u` [obs] |
| new-file hunks of the patch vs `~/pixelios-artifacts/localbuild/work/audio/xnu/bsd/dev/sweet_trusty.{c,h}` (`diff`) | identical [obs] |
| `llvm-nm K`, `llvm-objdump -d` of `_sweet_trusty_init` (0xfffffe0007546e00) | the wrapper and the probe are inlined; 3 `smc #0`; constants as in item 1; gate as in item 4; x4-x7 not written before the SMCs (F1) [obs] |
| hostile fakes, plain and `-fsanitize=address,undefined` | `PASS hostile` twice [obs] |
| 20 mutants against the committed test | 15 killed, 2 real survivors (F2), 3 equivalent [obs] |

## What I could not verify

- The EL3 monitor and Trusty binaries of this phone: how they treat x4-x7, whether entity-60 fastcalls from XNU at EL1 are routed, and what exactly `SMC_FC_API_VERSION` stores. My statements there are tagged [guess]; the stock driver source shows only the caller side.
- That a fastcall cannot hang in the monitor on this firmware. Stock Linux issues the same two calls on every boot; the difference here is the caller (XNU, after ABL and the shim), which is what the run tests.
- The kernel was not rebuilt by me (as instructed). I checked the given binary by strings and disassembly and the patched tree by `diff`; I did not prove that `K` was built from exactly that tree beyond the matching strings and code.
- Nothing was run on the phone.
