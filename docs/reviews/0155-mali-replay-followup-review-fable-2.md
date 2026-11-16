VERDICT: SAFE

# Follow-up delta review of xnu patch 0155 `sweet-mali-replay` (`sweetmali=0x100`), reviewer fable 2

Scope: `git diff 5083d32..63266d1 -- xnu/patches/0155-sweet-mali-replay.patch` (branch `wip/oriole-gpu`; the only commit in the range that
touches 0155 is dbe8b9d) plus the related tool/test changes of dbe8b9d (`tools/pack_gpujob.py`, `tools/test_pack_gpujob.py`,
`tools/test_mali_replay_mutants.py`, `shim/mali_replay_host_test.c`, `gpu/capture/run-capture.sh`, `gpu/mesa-patches/0004`), and the F1 hunk,
which is not in 0155 but in `xnu/patches/0157-sweet-mali-irq.patch` (commit a879ee9). Base: 0155 reviewed SAFE at 5083d32
(`docs/reviews/0155-mali-replay-review-fable-1.md`, F1-F8). I did not write the change, changed no code and ran nothing on the phone.
Reviewed 2026-11-16.

Tags: **[obs]** = I ran it and saw the result, **[src]** = read from source, **[guess]** = not verified.
`P:n` = line n of `xnu/patches/0155-sweet-mali-replay.patch` at 63266d1; `Q:n` = line n of `xnu/patches/0157-sweet-mali-irq.patch` at 63266d1.

DELTA B: SAFE

## What changed in patch 0155 (complete: 9 added code lines, the rest are index / hunk-header lines) [obs, `git diff`]

1. F3, in `sm_replay`, P:276-279:
   `+	if (PAGE_SIZE != SMR_BO_PAGE) {`
   `+		printf("SWEET MALI: replay: page size %lu is not 16 KiB, refused\n", (unsigned long)PAGE_SIZE);   /* review 0155 F3: smr_pa and the 4 x 4 KiB map loop assume 16 KiB */`
   `+		return;`
   `+	}`
2. F2, in `smr_job_parse` (`bsd/dev/sweet_mali_replay.h`), P:591-595:
   `+		for (k = 0; k < 24 && r[40 + k] != 0; k++) {`
   `+			if (r[40 + k] < 0x20u || r[40 + k] > 0x7eu) {`
   `+				return SMR_E_BO;        /* review 0155 F2: the label is printed; printable ASCII only */`
   `+			}`
   `+		}`
3. Bookkeeping: blob ids (`6aa9c23` -> `c882063`, `d7034c6` -> `b39ec5a`) and hunk lengths `713,265` -> `713,269` (+4 = F3),
   `985` -> `989`, `996` -> `1000`, `+1,376` -> `+1,381` (+5 = F2). The counts match the added lines [obs].

F1 is not part of this diff. It is in patch 0157, in `sm_replay_job`, Q:197-199:
   `+			if (rc != EINVAL) {`
   `+				safe = 0;       /* review 0155 F1 (needed once the IRQ path exists): ETIMEDOUT/EBUSY/ENXIO may mean a job is in flight, so leak, never free */`
   `+			}`

## Checks

- **Only refusals / stricter checks / safer cleanup: PASS** [src]. F3 is an early `return` before `ml_static_ptovirt`, before the container
  is parsed and before any allocation, map or submit. F2 is one more `return SMR_E_BO` in the parser; a refused job sets `*stop` in
  `sm_replay_job` before any BO exists (P:130-135). F1 only turns `safe` from 1 to 0, which can only suppress `sweet_mali_unmap` and
  `kmem_free` (P:223-236).
- **No new GPU register write: PASS** [obs + src]. `grep -c '^+.*\bwr('` on 0155 at 63266d1 = 0 (as at 5083d32); none of the 9 added lines
  touches a register, a map, or an interrupt mask. The F1 hunk of 0157 adds no register access either.
- **VA / bounds checks only tightened: PASS** [src]. The existing BO condition (P:586-590) and `mali_va_reserve` (P:692 ff.) are unchanged
  context; F2 adds a reject and accepts nothing that was refused before.
- **Label check cannot read out of bounds: PASS** [src]. `r = buf + bo_off + 64 * i` (P:583); the loop reads `r[40..63]` at most, inside the
  64-byte BO record. The whole BO table was already proven inside the file (`smr_in_file(bo_off, 64 * n_bos, len)`, P:553,
  unchanged), and `smr_job_bo` on the line before reads the same 24 bytes (P:496-498). The loop is also bounded by `k < 24`
  independently of the NUL; `r[63] != 0` was refused just above. `r` is `const uint8_t *`, so bytes >= 0x80 compare as unsigned and are
  refused (host test 0xc3 case). `k` is the function's existing unsigned index, re-initialised by the later overlap loop.
- **F3 placement: PASS** [src]. `PAGE_SIZE` is cast to `unsigned long` for `%lu`. With 16 KiB pages (this port) the check is always false:
  no behaviour change on the phone. It covers F3's overrun of `smr_pa` (`n = b.size / PAGE_SIZE`, P:147) because nothing after it runs.
- **F1 cannot double-free, and its leak is bounded: PASS** [src]. `sweet_mali_submit` (read in the patched tree
  `~/pixelios-artifacts/localbuild/work/gpu/xnu/bsd/dev/sweet_mali.c:543-590`) returns `ENXIO` (not ready / dead), `EINVAL` (jc outside
  the window or misaligned; checked before the lock, before any register access), `ETIMEDOUT` (`sm.busy` stayed set), `EBUSY` (slot not
  clean) or 0. `EINVAL` therefore proves this call started nothing, and every earlier chain of the job was waited to completion or left
  through `goto out` (P:181-203), so unmap + free stays correct for it. For the other codes `safe = 0`: the `out:` loop then skips
  `sweet_mali_unmap` (short-circuit on `safe`), skips `kmem_free`, prints "leaked on purpose" and zeroes `smr_bo[i].kva` (P:223-236). Each
  `kva` is freed at most once: either in the map-refusal branch (which zeroes it, P:168-169) or in the `out:` loop (which zeroes it). The
  leak is at most one job's BOs (`SMR_MAX_JOB_BYTES` = 64 MiB wired, plus its GPU VA range) until reboot, once per boot: `!safe` sets `*stop`
  (P:237-239) and the replay loop ends. On a test kernel that is the intended trade (leak instead of freeing pages a running job may write).
- **Hardware evidence** [src, `device/google/oriole/docs/PORT.md:104`]: kernel `xnu-local-lane-g-63266d1` (contains F2, F3 and, through 0157,
  F1), run gpu-o3-g4-irq2-replay: `replay clear=PASS crc=0xff12cb5c (expected)`, `replay tri=PASS crc=0x7a47b674 (expected)`,
  `replay done 2/2 PASS`. I did not see the log itself, only the record.

## Tool / test changes of dbe8b9d [src + obs]

- `tools/pack_gpujob.py:55-57`: the label bytes up to the first NUL must be 0x20..0x7e, else `ValueError('bo i label')`. Same rule as the
  kernel; the slice is `b[off+40:off+64]` on a record already proven inside the file by `inside(bo_off, 64 * nbo)`. Bytes after the NUL
  are ignored on both sides (host test case `f[e + 45] = 0x0a` after a NUL: accepted).
- `tools/test_pack_gpujob.py`: three bad labels (`\n`, 0x7f, 0xc3). `python3 -I tools/test_pack_gpujob.py` -> `Ran 7 tests ... OK` [obs].
- `shim/mali_replay_host_test.c`: 0x0a, 0x7f, 0xc3 refused; 0x7e accepted; a control byte after the NUL accepted.
- `tools/test_mali_replay_mutants.py`: one more mutant ("bo label printable (F2)"). `python3 -B tools/test_mali_replay_mutants.py` ->
  49 `killed` lines, `Ran 2 tests ... OK`, exit 0 [obs].
- F4: `sweet_malijob --crc CRC` (mesa patch 0004, host tool) and `gpu/capture/run-capture.sh` passes `--crc 0x7a47b674` for `tri`. Host
  side only; kernel code unchanged. Effect on the phone: `smr_rt_check` already counts a CRC mismatch as bad when `crc_valid` is set
  (P:679-681), so with the new capture `tri` PASS now also requires crc 0x7a47b674 (stricter). Not rebuilt or re-captured by me
  [guess: the recorded sha256 values in note 52 s.10].

## Findings

| id | sev | finding | fix |
|---|---|---|---|
| B1 | L | F1 is implemented in patch 0157 (Q:197-199), not in 0155: a kernel built with patches up to 0155 only still frees after `ETIMEDOUT`/`EBUSY`/`ENXIO` from submit. Review 1 F1 states this is unreachable while the replay is the only submitter, and 0157 is where the second path appears, so the placement is consistent; the hunk was also seen by `docs/reviews/0157-mali-irq-review-fable-1.md` ("Review 0155 F1: fixed"). | None required. Keep 0155 and 0157 together in any series (or move the 3 lines into 0155 on the next rebase). |
| B2 | info | F5 (print JS_HEAD / JS_TAIL) not done, as documented in note 52 s.10; F6-F8 needed no change. | None. |

Neither has to be fixed before the branch is pushed for CI.
