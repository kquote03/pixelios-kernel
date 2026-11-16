#!/usr/bin/env python3
"""Mutation check for xnu patch 0158 (bsd/dev/sweet_amp.h, review 0158-amp-init-review-fable-1 F2): each mutant of the header must make
shim/amp_xnu_host_test.c fail. Extracts the header from the patch, applies one mutation at a time, compiles and runs the test.
Usage (from the repo root, in the FHS env): python3 tools/amp_xnu_mutants.py      Last line: RESULT: PASS n/n mutants killed | FAIL ..."""
import pathlib, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCH = ROOT / "xnu/patches/0158-sweet-amp-init.patch"
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
    # removing the by-name PWR_CTRL1 refusal alone is an equivalent mutant (0x2014 is not in AMP_INIT_SEQ either); the 13th-pair mutant shows the refusal works
    ("a 13th pair", "X(AMP_REG_CCM_CORE, 0u)", "X(AMP_REG_CCM_CORE, 0u) X(0x2014u, 1u)"),
]


def header() -> str:
    out, on = [], False
    for ln in PATCH.read_text().splitlines():
        if ln == "+++ b/bsd/dev/sweet_amp.h":
            on = True
            continue
        if on and (ln.startswith("diff ") or ln.startswith("--- ")):
            break
        if on and ln.startswith("@@"):
            continue
        if on and ln.startswith("+"):
            out.append(ln[1:])
    return "\n".join(out) + "\n"


def run(hdr: str, d: pathlib.Path) -> bool:
    (d / "sweet_amp.h").write_text(hdr)
    c = subprocess.run(["gcc", "-O1", "-w", f"-I{d}", "-o", str(d / "t"), str(TEST)], capture_output=True, text=True)
    if c.returncode:
        return False	# a mutant that does not compile counts as killed
    return subprocess.run([str(d / "t")], capture_output=True, text=True).returncode == 0


def main() -> int:
    good = header()
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
