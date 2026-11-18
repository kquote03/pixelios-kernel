#!/usr/bin/env python3
"""Mutation check for xnu patch 0158 (bsd/dev/sweet_amp.h, review 0158-amp-init-review-fable-1 F2): each mutant of the header must make
shim/amp_xnu_host_test.c fail. Extracts the header from the patch, applies one mutation at a time, compiles and runs the test.
Usage (from the repo root, in the FHS env): python3 tools/amp_xnu_mutants.py      Last line: RESULT: PASS n/n mutants killed | FAIL ..."""
import pathlib, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCH = ROOT / "xnu/patches/0158-sweet-amp-init.patch"
PATCH_A9 = ROOT / "xnu/patches/0166-aoc-audio.patch"	# patch 0166 (A9b) extends sweet_amp.h: its hunk is applied on top
PATCH_PLAY = ROOT / "xnu/patches/0167-aoc-playback.patch"	# patch 0167 (sweetaoc=5) extends sweet_amp.h and sweet_audio.h again
PATCH_MATCH = ROOT / "xnu/patches/0168-aoc-reply-match.patch"
PATCH_DSP = ROOT / "xnu/patches/0169-amp-dsp-load.patch"	# patch 0169 (sweetaoc=6): sweet_amp.h hunk + the new sweet_amp_dsp.h	# patch 0168: sweet_audio.h reply matching (id not compared, as stock)
PATCH_CHUNK = ROOT / "xnu/patches/0170-amp-dsp-chunk.patch"	# patch 0170 (A9f): sweet_amp_dsp.h bursts of <= 480 bytes + failure detail
PATCH_MPU = ROOT / "xnu/patches/0171-amp-dsp-mpu.patch"	# patch 0171 (A9g): MPU read-back logged, relock after a failed MPU write, detail lines
TEST_AUDIO = ROOT / "shim/aoc_audio_host_test.c"
TEST = ROOT / "shim/amp_xnu_host_test.c"

MUTANTS = [	# (label, old, new): the reviewer's five survivors (m08 m09 m10 m12 m13) + the round-2 fixes
    ("gate without l.otp / OTP_BOOT_ERR", "((ee) == AMP_OK && (rr).otp && !((rr).st3 & AMP_OTP_BOOT_ERR) &&", "((ee) == AMP_OK &&"),
    ("gate without OTP_BOOT_ERR", " && !((rr).st3 & AMP_OTP_BOOT_ERR) && (rr).devid", " && (rr).devid"),
    ("gate without revid", " && (rr).devid == AMP_DEVID && (rr).revid == AMP_REVID)", " && (rr).devid == AMP_DEVID)"),
    ("BST_DCM_UVP_ERR dropped", "0x100u | 0x80u | 0x40u)", "0x100u | 0x40u)"),
    ("bit 16 dropped (F6)", "0x20000u | 0x10000u | 0x8000u", "0x20000u | 0x8000u"),
    ("read-backs not compared", "mask == AMP_INT1_MASK_DEFAULT && ccm == 0u ? AMP_OK", "1 ? AMP_OK"),
    ("no other-CS check in amp_write", "\tif (!r32(odat, &o) || !peri_pin_bit(o, onr))\n\t\treturn AMP_ERROR;\t\t/* never both amps on the bus */\n", ""),
    ("no pre-write error check (F6)", "\tif (e == AMP_OK && (st0 & AMP_ERR_BITS)) {", "\tif (0) {"),
    ("R written after a failed L (F4)", "\t\tif (il == AMP_OK && AMP_ID_OK(er, r))", "\t\tif (AMP_ID_OK(er, r))"),
    ("pass line from another condition (F1)", 'con_str("SWEET", pass ? "AMP: init ok', 'con_str("SWEET", 1 ? "AMP: init ok'),
    # review 0158 round 2 N2: the post-write check and read, each error bit, the DEVID gate
    ("post-write error check removed (n01)", "\treturn e == AMP_OK && !(st1 & AMP_ERR_BITS) && mask", "\treturn e == AMP_OK && mask"),
    ("post-write status read removed (n20)", "\tif (e == AMP_OK)\n\t\te = amp_read(s, cs, AMP_REG_IRQ1_STATUS1, &st1);\n", ""),
    ("AMP_SHORT_ERR dropped (n11)", "(0x80000000u | 0x20000u |", "(0x20000u |"),
    ("TEMP_ERR dropped (n17)", "0x80000000u | 0x20000u | 0x10000u", "0x80000000u | 0x10000u"),
    ("TEMP_WARN dropped (n10)", "0x10000u | 0x8000u | 0x100u", "0x10000u | 0x100u"),
    ("BST_SHORT_ERR dropped (n18)", "0x8000u | 0x100u | 0x80u", "0x8000u | 0x80u"),
    ("BST_OVP_ERR dropped", "0x80u | 0x40u)", "0x80u)"),
    ("gate without devid (m15)", " && (rr).devid == AMP_DEVID && (rr).revid", " && (rr).revid"),
    # removing the by-name PWR_CTRL1 refusal alone is an equivalent mutant (0x2014 is not in AMP_INIT_SEQ either); the 13th-pair mutant shows the refusal works
    ("a 13th pair", "X(AMP_REG_CCM_CORE, 0u)", "X(AMP_REG_CCM_CORE, 0u) X(0x2014u, 1u)"),
    # A7 OTP unpack: the unlock / relock path only
    ("no relock after a failed unpack", "\tif (unlocked) {\n\t\tlk = ", "\tif (unlocked && e == AMP_OK) {\n\t\tlk = "),
    ("second lock word dropped", "\t\tlk = (amp_write(s, cs, AMP_REG_TEST_KEY, AMP_OTP_KEY_LOCK2) == AMP_OK) && lk;\n", ""),
    ("a failed lock does not fail the amp", "return e == AMP_OK && unlocked && lk && nbad == 0u", "return e == AMP_OK && unlocked && nbad == 0u"),
    ("unlock without its second word", "\t\tif (e == AMP_OK)\n\t\t\te = amp_write(s, cs, AMP_REG_TEST_KEY, AMP_OTP_KEY_UNLOCK2);\n", ""),
    # patch 0166 A9b (new stop / restore paths)
    # dropping only the by-name PWR_CTRL1 refusal is equivalent (no row names 0x2014: _Static_assert + reg == t->reg), so it is paired with the row check
    ("A9: PWR_CTRL1 allowed while armed (and row register unchecked)", "if (!amp_a9.on || amp_a9.idx >= AMP_A9_N || reg == AMP_REG_PWR_CTRL1)\n\t\treturn 0;\n\tt = &amp_a9_tab[amp_a9.idx];\n\treturn reg == t->reg && ", "if (!amp_a9.on || amp_a9.idx >= AMP_A9_N)\n\t\treturn 0;\n\tt = &amp_a9_tab[amp_a9.idx];\n\treturn "),
    ("A9: row register not checked", "\treturn reg == t->reg && (val & t->mask)", "\treturn (val & t->mask)"),
    ("A9: bits outside the mask not checked", " && (val & ~t->mask) == (amp_a9.old & ~t->mask);", ";"),
    ("A9: GLOBAL_EN before the config not checked", "((d->st1 & AMP_ERR_BITS) || (d->pwr1 & AMP_GLOBAL_EN))", "(d->st1 & AMP_ERR_BITS)"),
    ("A9: AOC close skipped", "\tamp_a9_cb.close(amp_a9_cb.ctx);", "\tif (0) amp_a9_cb.close(amp_a9_cb.ctx);"),
    ("A9: PLL poll does not stop at the lock", "d->polls < AMP_A9_PLL_POLLS && !d->locked", "d->polls < AMP_A9_PLL_POLLS"),
]

# patch 0169 A9e: the new stop / restore path of sweet_amp_dsp.h and its hook in sweet_amp.h (killed by the A9e cases of amp_xnu_host_test)
MUTANTS_DSP = [
    ("A9e: started core never stopped", "\tif (!d->started || d->stopped)\n\t\treturn;\n\td->stopped = 1;", "\tif (1)\n\t\treturn;\n\td->stopped = 1;"),
    ("A9e: started marked only after the CCM write", "\td->started = 1;\n\te = amp_dsp_rmw(", "\te = amp_dsp_rmw("),
    ("A9e: error bit after the start not checked", "(d->st1 & AMP_ERR_BITS) || (d->pwr1 & AMP_GLOBAL_EN))\n\t\t\td->stage = DS_ERR;", "0)\n\t\t\td->stage = DS_ERR;"),
    ("A9e: read-back not compared", "if ((!unp24 || (i & 3u)) && amp_dsp_rb[i] != d[off + i])", "if (0)"),
    # patch 0171 A9g: the relock after a failed MPU write (new restore path); the MPU read-back stays a logged value
    ("A9g: no relock after a failed MPU write", "\telse\n\t\t(void)amp_write(s, cs, AMP_DSP_MPU_LOCK, 0u);", "\telse\n\t\t(void)0;"),
    ("A9g: MPU read-back mismatch stops again", "\t\td->mpu_rb++;\n", "\t\tif (rb != 0xFFFFFFFFu)\n\t\t\treturn AMP_ERROR;\n\t\td->mpu_rb++;\n"),
]
MUTANTS_HOOK = [
    ("A9e: start with only L loaded", "amp_dsp_res.a[0].stage == DS_LOADED && amp_dsp_res.a[1].stage == DS_LOADED", "amp_dsp_res.a[0].stage == DS_LOADED"),
    ("A9e: R loaded after a failed L", "if (amp_dsp_load(s, 0, &amp_dsp_res.a[0]) == AMP_OK)\n\t\t\t(void)", "(void)amp_dsp_load(s, 0, &amp_dsp_res.a[0]);\n\t\t\t(void)"),
]

# patch 0167: the new stop path of sweet_audio.h (SOURCE on 0 before the BIND2 undo), killed by shim/aoc_audio_host_test.c
MUTANTS_AUDIO = [
    ("play: SOURCE off never sent", "\tif (r->started && !r->stopped) {", "\tif (0) {"),
    ("play: started only on a good reply (no stop after a timeout / reject)", "r->started = r->x[r->nx - 1u].written;", "r->started = rc == SWAU_OK;"),
    ("play: stop not marked (SOURCE off sent again)", "\t\tr->stopped = 1;\n", ""),
]


def header(name: str = "sweet_amp.h", patch: pathlib.Path = PATCH) -> str:
    out, on = [], False
    for ln in patch.read_text().splitlines():
        if ln == f"+++ b/bsd/dev/{name}":
            on = True
            continue
        if on and (ln.startswith("diff ") or ln.startswith("--- ")):
            break
        if on and ln.startswith("@@"):
            continue
        if on and ln.startswith("+"):
            out.append(ln[1:])
    return "\n".join(out) + "\n"


def hunk(patch: pathlib.Path, name: str) -> str:
    diff, on = [], False
    for ln in patch.read_text().splitlines(keepends=True):
        if ln.rstrip("\n") == f"diff --git a/bsd/dev/{name} b/bsd/dev/{name}":
            on = True
        elif on and ln.startswith("diff --git"):
            break
        if on:
            diff.append(ln)
    if not diff:
        raise SystemExit(f"RESULT: FAIL no {name} hunk in " + patch.name)
    return "".join(diff)


def apply(base: str, name: str, patches) -> str:
    """base with the `name` hunks of the later patches applied on top, in order (as shim/build.sh does)."""
    with tempfile.TemporaryDirectory() as td:
        d = pathlib.Path(td)
        (d / "bsd/dev").mkdir(parents=True)
        (d / f"bsd/dev/{name}").write_text(base)
        for p in patches:
            (d / "p.diff").write_text(hunk(p, name))
            c = subprocess.run(["patch", "-s", "-p1", "--no-backup-if-mismatch", "-i", "p.diff"], cwd=d, capture_output=True, text=True)
            if c.returncode:
                raise SystemExit(f"RESULT: FAIL {p.name} does not apply to {name}: " + c.stdout + c.stderr)
        return (d / f"bsd/dev/{name}").read_text()


def header_dsp() -> str:
    """sweet_amp_dsp.h of patch 0169 with the 0170 hunk applied on top (as shim/build.sh does)."""
    return apply(header("sweet_amp_dsp.h", PATCH_DSP), "sweet_amp_dsp.h", [PATCH_CHUNK, PATCH_MPU])


def header_a9() -> str:
    """The 0158 header with the sweet_amp.h hunks of patches 0166 and 0167 applied on top."""
    return apply(header(), "sweet_amp.h", [PATCH_A9, PATCH_PLAY, PATCH_DSP])


def run(hdr: str, d: pathlib.Path) -> bool:
    (d / "sweet_amp.h").write_text(hdr)
    (d / "sweet_amp_otp.h").write_text(header("sweet_amp_otp.h"))
    (d / "sweet_cs_dsp.h").write_text(header("sweet_cs_dsp.h", PATCH_A9))
    if not (d / "sweet_amp_dsp.h").exists() or not getattr(run, "dsp_mut", False):
        (d / "sweet_amp_dsp.h").write_text(header_dsp())
    c = subprocess.run(["gcc", "-O1", "-w", f"-I{d}", "-o", str(d / "t"), str(TEST)], capture_output=True, text=True)
    if c.returncode:
        return False	# a mutant that does not compile counts as killed
    return subprocess.run([str(d / "t")], capture_output=True, text=True).returncode == 0


def run_audio(hdr: str, d: pathlib.Path) -> bool:
    (d / "sweet_audio.h").write_text(hdr)
    (d / "sweet_cs_dsp.h").write_text(header("sweet_cs_dsp.h", PATCH_A9))
    c = subprocess.run(["gcc", "-O1", "-w", f"-I{d}", "-o", str(d / "ta"), str(TEST_AUDIO)], capture_output=True, text=True)
    if c.returncode:
        return False
    return subprocess.run([str(d / "ta")], capture_output=True, text=True).returncode == 0


def sweep(good: str, mutants, runner, d: pathlib.Path):
    killed = 0
    for label, old, new in mutants:
        if good.count(old) != 1:
            print(f"RESULT: FAIL mutant '{label}': pattern found {good.count(old)} times")
            return None
        if runner(good.replace(old, new), d):
            print(f"SURVIVED: {label}")
        else:
            killed += 1
            print(f"killed: {label}")
    return killed


def main() -> int:
    good = header_a9()
    good_audio = apply(header("sweet_audio.h", PATCH_A9), "sweet_audio.h", [PATCH_PLAY, PATCH_MATCH])
    with tempfile.TemporaryDirectory() as td:
        d = pathlib.Path(td)
        if not run(good, d) or not run_audio(good_audio, d):
            print("RESULT: FAIL the unmutated headers do not pass the host tests")
            return 1
        k1 = sweep(good, MUTANTS, run, d)
        k2 = sweep(good_audio, MUTANTS_AUDIO, run_audio, d)
        k3 = sweep(good, MUTANTS_HOOK, run, d)
        good_dsp = header_dsp()

        def run_dsp(hdr, dd):
            (dd / "sweet_amp_dsp.h").write_text(hdr)
            run.dsp_mut = True
            try:
                return run(good, dd)
            finally:
                run.dsp_mut = False
        k4 = sweep(good_dsp, MUTANTS_DSP, run_dsp, d)
        if k1 is None or k2 is None or k3 is None or k4 is None:
            return 1
    n = len(MUTANTS) + len(MUTANTS_AUDIO) + len(MUTANTS_HOOK) + len(MUTANTS_DSP)
    k1 = k1 + k3 + k4
    ok = k1 + k2 == n
    print(f"RESULT: {'PASS' if ok else 'FAIL'} {k1 + k2}/{n} mutants killed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
