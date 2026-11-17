#!/usr/bin/env python3
"""Mutation check for xnu patch 0158 (bsd/dev/sweet_amp.h, review 0158-amp-init-review-fable-1 F2): each mutant of the header must make
shim/amp_xnu_host_test.c fail. Extracts the header from the patch, applies one mutation at a time, compiles and runs the test.
Usage (from the repo root, in the FHS env): python3 tools/amp_xnu_mutants.py      Last line: RESULT: PASS n/n mutants killed | FAIL ..."""
import pathlib, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCH = ROOT / "xnu/patches/0158-sweet-amp-init.patch"
PATCH_A9 = ROOT / "xnu/patches/0166-aoc-audio.patch"	# patch 0166 (A9b) extends sweet_amp.h: its hunk is applied on top
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


def header(name: str = "sweet_amp.h") -> str:
    out, on = [], False
    for ln in PATCH.read_text().splitlines():
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


def header_a9() -> str:
    """The 0158 header with the sweet_amp.h hunk of patch 0166 applied on top (as shim/build.sh does)."""
    diff, on = [], False
    for ln in PATCH_A9.read_text().splitlines(keepends=True):
        if ln.rstrip("\n") == "diff --git a/bsd/dev/sweet_amp.h b/bsd/dev/sweet_amp.h":
            on = True
        elif on and ln.startswith("diff --git"):
            break
        if on:
            diff.append(ln)
    if not diff:
        raise SystemExit("RESULT: FAIL no sweet_amp.h hunk in " + PATCH_A9.name)
    with tempfile.TemporaryDirectory() as td:
        d = pathlib.Path(td)
        (d / "bsd/dev").mkdir(parents=True)
        (d / "bsd/dev/sweet_amp.h").write_text(header())
        (d / "p.diff").write_text("".join(diff))
        c = subprocess.run(["patch", "-s", "-p1", "--no-backup-if-mismatch", "-i", "p.diff"], cwd=d, capture_output=True, text=True)
        if c.returncode:
            raise SystemExit("RESULT: FAIL 0166 does not apply to sweet_amp.h: " + c.stdout + c.stderr)
        return (d / "bsd/dev/sweet_amp.h").read_text()


def run(hdr: str, d: pathlib.Path) -> bool:
    (d / "sweet_amp.h").write_text(hdr)
    (d / "sweet_amp_otp.h").write_text(header("sweet_amp_otp.h"))
    c = subprocess.run(["gcc", "-O1", "-w", f"-I{d}", "-o", str(d / "t"), str(TEST)], capture_output=True, text=True)
    if c.returncode:
        return False	# a mutant that does not compile counts as killed
    return subprocess.run([str(d / "t")], capture_output=True, text=True).returncode == 0


def main() -> int:
    good = header_a9()
    with tempfile.TemporaryDirectory() as td:
        d = pathlib.Path(td)
        if not run(good, d):
            print("RESULT: FAIL the unmutated header does not pass the host test")
            return 1
        killed = 0
        for label, old, new in MUTANTS:
            if good.count(old) != 1:
                print(f"RESULT: FAIL mutant '{label}': pattern found {good.count(old)} times")
                return 1
            if run(good.replace(old, new), d):
                print(f"SURVIVED: {label}")
            else:
                killed += 1
                print(f"killed: {label}")
    ok = killed == len(MUTANTS)
    print(f"RESULT: {'PASS' if ok else 'FAIL'} {killed}/{len(MUTANTS)} mutants killed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
