"""tools/mkpayload.py refuses a board whose board.json still lists provisional keys (CR-02 killing test)."""
import os
import sys
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import mkpayload  # noqa: E402

MISSING = "/nonexistent/kernel.development.sweet"


class Provisional(unittest.TestCase):
    def test_refused_before_any_file_is_read(self):
        with mock.patch.object(mkpayload.boards, "provisional", return_value=["dram_banks"]):
            with self.assertRaises(SystemExit) as cm:
                mkpayload.build(MISSING, MISSING, "", board="oriole")
        self.assertIn("provisional", str(cm.exception))
        self.assertIn("dram_banks", str(cm.exception))

    def test_measured_board_passes_the_check(self):
        with mock.patch.object(mkpayload.boards, "provisional", return_value=[]):
            with self.assertRaises(FileNotFoundError):     # got past the check, failed on the missing kernel
                mkpayload.build(MISSING, MISSING, "", board="bluejay")

    def test_cli_accepts_every_board(self):
        with open(os.path.join(ROOT, "tools", "mkpayload.py")) as f:
            self.assertNotIn("choices=('sweet', 'bluejay')", f.read())


if __name__ == "__main__":
    unittest.main()
