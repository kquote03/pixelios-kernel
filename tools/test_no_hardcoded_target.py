"""Phone-touching scripts take the target from tools/boards.py --target, never a typed codename (code lines only, comments ignored)."""
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = ("tools/fboot.sh", "tools/fflash-userdata.sh", "tools/pstore-pull.py", "tools/probe-run", "tools/xrun",
         "tools/build-xnu-image.sh", "tools/run-probe.sh")


def code_lines(path):
    with open(os.path.join(ROOT, path)) as f:
        for n, line in enumerate(f, 1):
            if not line.lstrip().startswith("#"):
                yield n, line


class NoTypedTarget(unittest.TestCase):
    def test_no_bluejay_literal(self):
        bad = [f"{f}:{n}: {line.strip()}" for f in FILES for n, line in code_lines(f) if re.search(r"\bbluejay\b", line)]
        self.assertEqual(bad, [])

    def test_each_reads_the_target(self):
        for f in FILES:
            with open(os.path.join(ROOT, f)) as fh:
                self.assertRegex(fh.read(), r"boards\.py\"? --target|target_board\(\)", f)

    def test_the_check_can_fail(self):    # CR-01
        self.assertTrue(re.search(r"\bbluejay\b", '[ "$prod" = bluejay ] || exit 1'))


if __name__ == "__main__":
    unittest.main()
