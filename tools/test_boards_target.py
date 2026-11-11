"""tools/boards.py target_board() / provisional() / device_name(): the one source of the phone the tools may touch (AGENTS.md rule 2)."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import boards  # noqa: E402


class TargetBoard(unittest.TestCase):
    def test_default_is_bluejay(self):
        self.assertEqual(boards.target_board({}), "bluejay")

    def test_empty_means_default(self):
        self.assertEqual(boards.target_board({"BOARD": ""}), "bluejay")

    def test_unknown_is_refused(self):
        with self.assertRaises(SystemExit):
            boards.target_board({"BOARD": "raven"})

    def test_qemu_only_board_is_refused(self):
        with self.assertRaises(SystemExit):        # sweet is a QEMU-only legacy tree, never a phone target
            boards.target_board({"BOARD": "sweet"})

    def _cli(self, board):
        env = dict(os.environ, BOARD=board)
        return subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "boards.py"), "--target"], env=env,
                              capture_output=True, text=True)

    def test_cli_prints_target(self):
        r = self._cli("bluejay")
        self.assertEqual((r.returncode, r.stdout), (0, "bluejay\n"))

    def test_cli_refuses_unknown(self):
        r = self._cli("nope")
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(r.stdout, "")
        self.assertIn("nope", r.stderr)


class Provisional(unittest.TestCase):
    def _tree(self, provisional):
        d = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, d)
        os.makedirs(os.path.join(d, "soc", "v", "s"))
        os.makedirs(os.path.join(d, "device", "v", "x"))
        with open(os.path.join(d, "soc", "v", "s", "soc.json"), "w") as f:
            json.dump({"name": "s"}, f)
        dev = {"name": "X", "codename": "x", "soc": "v/s"}
        if provisional is not None:
            dev["provisional"] = provisional
        with open(os.path.join(d, "device", "v", "x", "board.json"), "w") as f:
            json.dump(dev, f)
        return boards.load_devices(d)

    def test_lists_the_keys(self):
        self.assertEqual(boards.provisional("x", self._tree(["dram_banks"])), ["dram_banks"])

    def test_absent_means_none(self):
        self.assertEqual(boards.provisional("x", self._tree(None)), [])

    def test_not_a_build_value(self):
        self.assertNotIn("provisional", self._tree(["dram_banks"])["x"][3])

    def test_bluejay_is_fully_measured(self):
        self.assertEqual(boards.provisional("bluejay"), [])

    def test_device_name(self):
        self.assertEqual(boards.device_name("bluejay"), "Pixel 6a")


if __name__ == "__main__":
    unittest.main()
