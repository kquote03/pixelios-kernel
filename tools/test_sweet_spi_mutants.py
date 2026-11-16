"""Mutation test of the GIC SPI rules in xnu patch 0157 (pexpert/arm/sweet_spi.h, plan oneshot-2 G4, docs/research/35 s.8).

Each rule (allowlist 452/453, attach preconditions, mask range, slot rules, unhandled counting) is removed in a copy of the header; the host
test shim/sweet_spi_host_test.c (ASan/UBSan build) must FAIL against every copy. A survivor or a pattern that matches nothing fails this test.
Run: python3 -B tools/test_sweet_spi_mutants.py"""
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH = os.path.join(ROOT, "xnu", "patches", "0157-sweet-mali-irq.patch")
TEST = os.path.join(ROOT, "shim", "sweet_spi_host_test.c")
PATH = "pexpert/arm/sweet_spi.h"

MUTANTS = [
    ("allow 451", "return intid == 452u || intid == 453u;", "return intid >= 451u && intid <= 453u;"),
    ("allow any", "return intid == 452u || intid == 453u;", "return 1;"),
    ("edge accepted", "if ((s->icfgr >> r.icfgr_shift) & 2u) {", "if (0) {"),
    ("group modifier accepted", "if (s->igrpmodr & r.bit) {", "if (0) {"),
    ("IRM accepted", "if ((s->irouter & SSPI_IROUTER_IRM) || ", "if ("),
    ("other CPU accepted", " || (s->irouter & 0xFF00FFFFFFull) != s->mpidr_aff) {", ") {"),
    ("precheck ignores allowlist", "\tif (sweet_spi_regs(intid, &r)) {\n\t\treturn -1;\n\t}\n\tif ((s->icfgr", "\t(void)sweet_spi_regs(intid, &r);\n\tif ((s->icfgr"),
    ("mask below 32", "if (intid < 32u || intid >= 1020u) {\n\t\treturn -1;", "if (intid >= 1020u) {\n\t\treturn -1;"),
    ("mask special ids", "if (intid < 32u || intid >= 1020u) {\n\t\treturn -1;", "if (intid < 32u) {\n\t\treturn -1;"),
    ("set without handler", "if (!sweet_spi_allowed(intid) || fn == NULL) {", "if (!sweet_spi_allowed(intid)) {"),
    ("set not allowlisted", "if (!sweet_spi_allowed(intid) || fn == NULL) {", "if (fn == NULL) {"),
    ("set duplicate", "if (t->slot[i].fn != NULL && t->slot[i].intid == intid) {\n\t\t\treturn -1;", "if (0) {\n\t\t\treturn -1;"),
    ("dispatch PPIs", "if (intid < 32u || intid >= 1020u) {\n\t\treturn SSPI_NOT_SPI;", "if (intid >= 1020u) {\n\t\treturn SSPI_NOT_SPI;"),
    ("no unhandled count", "\tt->unhandled++;\n", ""),
    ("no last unhandled", "\tt->last_unhandled = intid;\n", ""),
    ("affinity keeps MT", "return mpidr & 0xFF00FFFFFFull;", "return mpidr & 0xFFFFFFFFFFull;"),
    ("F1 require IGROUPR", "\t(void)igroupr;\n\treturn (isen & bit) != 0;", "\treturn (isen & bit) != 0 && (igroupr & bit) != 0;"),
    ("F1 ignore ISENABLER", "\t(void)igroupr;\n\treturn (isen & bit) != 0;", "\t(void)igroupr;\n\t(void)isen;\n\treturn bit != 0;"),
    ("F2 no storm limit", "if (++t->slot[i].notmine < SSPI_STORM_MAX) {", "if (++t->slot[i].notmine || 1) {"),
    ("F2 no reset on mine", "\t\t\t\tt->slot[i].notmine = 0;\n\t\t\t\treturn SSPI_HANDLED;", "\t\t\t\treturn SSPI_HANDLED;"),
    ("F2 slot kept after storm", "\t\t\tt->slot[i].fn = NULL;    /* review 0157 F2", "\t\t\t(void)0;    /* review 0157 F2"),
    ("F2 storms not counted", "\t\t\tt->storms++;\n", ""),
]


def header():
    out, on = [], False
    with open(PATCH, encoding="utf-8") as fh:
        for line in fh.read().split("\n"):
            if line == "+++ b/" + PATH:
                on = True
                continue
            if on and (line.startswith("--- ") or line.startswith("diff ")):
                break
            if on and line.startswith("@@"):
                continue
            if on and line.startswith("+"):
                out.append(line[1:])
    return "\n".join(out) + "\n"


def run(text):
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "sweet_spi.h"), "w", encoding="utf-8") as fh:
            fh.write(text)
        exe = os.path.join(d, "t")
        c = subprocess.run(["gcc", "-O1", "-g", "-w", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + d, "-o", exe, TEST],
                           capture_output=True)
        if c.returncode != 0:
            return "killed(compile)"
        return "survived" if subprocess.run([exe], capture_output=True).returncode == 0 else "killed"


class SpiMutants(unittest.TestCase):
    def test_unmutated_passes(self):
        self.assertEqual(run(header()), "survived")

    def test_all_killed(self):
        h = header()
        bad = []
        for name, pat, rep in MUTANTS:
            if pat not in h:
                bad.append((name, "pattern-missing"))
                continue
            s = run(h.replace(pat, rep))
            print("%-28s %s" % (name, s))
            if s == "survived":
                bad.append((name, s))
        self.assertEqual(bad, [])


if __name__ == "__main__":
    unittest.main()
