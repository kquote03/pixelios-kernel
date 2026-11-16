VERDICT: SAFE

# Follow-up delta review of xnu patch 0158 `sweet-amp-init` (`sweetamp=1`), reviewer fable 3

Scope: `git diff 39d80d7..154eafd -- xnu/patches/0158-sweet-amp-init.patch` (branch `wip/oriole-audio`; the only commit in that range that
touches the patch is 7ea70ef) plus the non-patch files of 7ea70ef (`shim/amp_xnu_host_test.c`, `tools/amp_xnu_mutants.py`, `shim/build.sh`).
Base: patch reviewed SAFE at 39d80d7 (`docs/reviews/0158-amp-init-review-fable-2.md`, findings N1-N3). I did not write the change, changed
no code and ran nothing on the phone. Reviewed 2026-11-16.

Tags: **[obs]** = I ran it and saw the result, **[src]** = read from source, **[guess]** = not verified.
`P:n` = line n of `xnu/patches/0158-sweet-amp-init.patch` at 154eafd.

DELTA A: SAFE

## What changed in the patch (complete: 10 `-`/`+` lines in 4 hunks) [obs, `git diff`]

1. Patch description, P:3 (text only):
   `-(the port of the shim --amp-id, pk-amp shim/ampid.c 4c7be85, review SAFE round 2) and, on each amp ...`
   `+(the port of the shim --amp-id, pk-amp shim/ampid.c 4c7be85, review SAFE round 2; A1 PASS on hardware o5-a1-ampid) and, on each amp ...`
2. Patch description, P:6-8 (text only; old 3 lines, new 3 lines):
   `+12 (register, value) pairs; PWR_CTRL1 / GLOBAL_EN is never written (refused by name). IRQ1_STATUS1 error bits (seven) must be 0 before the first write and after the writes, then both resets go`
   `+low and every pad / controller / sysreg / mux word is restored and read back. Lines "SWEET AMP: ..."; pass "SWEET AMP: init ok (no otp unpack) l,r devid 35a40`
   `+rev b2, errors=0" (A7 PARTIAL), then "SWEET AMP: verdict PASS". Host test shim/amp_xnu_host_test.c (header extracted from this patch). BUILT, UNRUN; needs A1 PASS and an independent review first.`
3. Hunk header of the new file `bsd/dev/sweet_amp.h`, P:176: `@@ -0,0 +1,1004 @@` -> `@@ -0,0 +1,1007 @@` (+3 = one more comment line + two code
   lines; the count matches) [obs].
4. Header comment of `sweet_amp.h`, P:195-197 (comment only; 2 lines became 3):
   `+ * IRQ1_STATUS1 0x10010 is read before the first write (st0: any error bit = no write) and after the writes (st1); error bits AMP_SHORT_ERR 0x80000000,`
   `+ * TEMP_ERR 0x20000, bit 16 (unmasked by INT1_MASK_DEFAULT, unnamed), TEMP_WARN 0x8000, BST_SHORT_ERR 0x100, BST_DCM_UVP_ERR 0x80, BST_OVP_ERR 0x40`
   `+ * (cs35l41.h:124, :684-689) must all be 0. Then the usual restore: both resets low (the amps lose the writes), every`
5. The only code change, in `amp_init`, P:878-879:
   `+	p = put_txt(line, sizeof(line), p, " st0=");`
   `+	p = put_hex(line, sizeof(line), p, st0, 8);`

## Checks

- **Only a print and texts: PASS** [obs + src]. No added line contains `w32`, `pad_put`, `amp_write`, `amp_read`, `xfer`, a register address in
  code, or a change to `AMP_INIT_SEQ`, `amp_write_ok`, `AMP_ERR_BITS`, `AMP_ID_OK`. The two added statements only append to the local
  `char line[RAMLOG_LINE_MAX]` (P:858).
- **No new MMIO / SPI access: PASS** [src]. `st0` is not a new read: it is the local `uint32_t st0 = 0` (P:855) that the already reviewed
  pre-write read fills (`amp_read(s, cs, AMP_REG_IRQ1_STATUS1, &st0)`, P:861, review-1 F6). The print sits after all writes and read-backs
  (P:867-874) and before `emit` (P:887); it adds no bus traffic and changes no timing on the bus.
- **Gates, order, restore, return value unchanged: PASS** [src]. The pre-write gate (P:862-865), the write macro (P:866-868), the three
  read-backs (P:869-874), the post-write error report (P:888-889) and the return expression (P:890) are byte-identical context lines in the
  diff; nothing after P:879 moved except by +3 lines.
- **Format and arguments: PASS** [src]. There is no printf format string: `put_txt` (P:474-479) and `put_hex` (P:481) take
  `(dst, cap, pos, ...)`; the new calls use the same argument shape as the neighbouring `st1` pair, `st0` is `uint32_t`, 8 digits.
  If the pre-write read fails, `amp_read` sets `*val = 0` (P:808), so `st0` prints 00000000 with ` FAILED`; never uninitialised.
- **Buffer: PASS** [src]. Longest line: name `L`/`R` (P:1161, P:1167) 1 + ` init writes=` 13 + 2 digits + ` st0=` 13 + ` st1=` 13 + ` mask1=` 15 +
  ` ccm=` 13 + ` FAILED` 7 = 77 bytes < 88 (`emit` cap `RAMLOG_LINE_MAX - 8`, P:517-519); `put_txt`/`put_hex` are bounded by `cap` anyway, so
  the worst case of a longer line is truncation, not an overflow.
- **Nothing else in the patch moved: PASS** [obs]. The diff has exactly the 10 changed lines quoted above.

## Non-patch files of 7ea70ef (host only) [src + obs]

- `shim/amp_xnu_host_test.c`: fake-amp fields `devid`, `st1_late`, `st1_late_at`; new cases: each of the 7 error bits appearing after write 1
  or write 12 on each amp (28 runs: fail, reported, no pass line, no R write after L, exact restore), R DEVID 0x35B40 (no write to R),
  `st3 = 0x20000000` on both amps passes with 12 + 12 writes. One case label corrected ("before the init (pre-write read)"), one citation
  corrected (`cs35l41.c:4306-4308`). Test code only.
- `tools/amp_xnu_mutants.py`: 8 more mutants (post-write check, post-write read, 5 single error bits, DEVID gate). Run here:
  `python3 -B tools/amp_xnu_mutants.py` -> `RESULT: PASS 19/19 mutants killed` [obs].
- `shim/build.sh`: one `chk "amp_xnu_mutants" "$PYTHON" ../tools/amp_xnu_mutants.py` line after the ASan host test, inside the existing
  host-test branch. Build-time only; a surviving mutant fails the build.
- N1, N2, N3 of review fable 2 are implemented as claimed [src]. With the two older entries ("bit 16 dropped (F6)",
  "BST_DCM_UVP_ERR dropped") every one of the 7 error bits now has its own mutant.

## Findings

| id | sev | finding | fix |
|---|---|---|---|
| A1 | L | The kernel that passed on hardware (run o5-a7-ampinit) was `xnu-local-audio-39d80d7`, built before this delta: its recorded init line is `init writes=12 st1=00000000 mask1=7ffcfe3f ccm=00000000`, without `st0=` (`device/google/oriole/docs/PORT.md:102`) [src]. The `st0` print itself is therefore host-tested but not yet seen on the phone. No safety relevance (print only). I found no log matcher in the tree that depends on the old line shape [obs, `git grep "init writes="`]. | None before the push. On the next A7 run, confirm the line shows `st0=00000000`. |
| A2 | L | The patch description still ends with "BUILT, UNRUN; needs A1 PASS and an independent review first" (P:8), while the same paragraph now says A1 passed and the 39d80d7 patch has run. Stale text. | Reword on the next edit of the patch. |

Neither has to be fixed before the branch is pushed for CI.
