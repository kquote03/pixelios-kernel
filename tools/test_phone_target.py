"""tools/phone verify_bluejay_target(): exact match on the BOARD target (the old code accepted any output containing 'bluejay')."""
import importlib.machinery
import importlib.util
import os
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_phone(board):
    old = os.environ.get("BOARD")
    os.environ["BOARD"] = board
    try:
        loader = importlib.machinery.SourceFileLoader("phone_" + board, os.path.join(ROOT, "tools", "phone"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        mod = importlib.util.module_from_spec(spec)
        loader.exec_module(mod)
        return mod
    finally:
        if old is None:
            os.environ.pop("BOARD", None)
        else:
            os.environ["BOARD"] = old


class Target(unittest.TestCase):
    def adb(self, board, reported):
        p = load_phone(board)
        p.safe_device_cmd = lambda *a, **k: (0, reported + "\n", "")
        return p.verify_bluejay_target("adb")

    def fastboot(self, board, reported):
        p = load_phone(board)
        p.run_bounded = lambda *a, **k: (0, "Bus 003 Device 009: ID 18d1:4ee0 Google Inc.", "")
        p.safe_device_cmd = lambda *a, **k: (0, "", "product: %s\nFinished. Total time: 0.001s" % reported)
        return p.verify_bluejay_target("fastboot")

    def test_target_constant(self):
        self.assertEqual(load_phone("oriole").TARGET, "oriole")

    def test_adb_exact(self):
        self.assertTrue(self.adb("oriole", "oriole"))
        self.assertFalse(self.adb("oriole", "bluejay"))
        self.assertFalse(self.adb("oriole", "oriole2"))       # a substring match is refused
        self.assertTrue(self.adb("bluejay", "bluejay"))
        self.assertFalse(self.adb("bluejay", "oriole"))

    def test_fastboot_exact(self):
        self.assertTrue(self.fastboot("oriole", "oriole"))
        self.assertFalse(self.fastboot("oriole", "bluejay"))
        self.assertFalse(self.fastboot("bluejay", "oriole"))

    def test_bad_board_refuses_import(self):
        with self.assertRaises(SystemExit):
            load_phone("raven")


if __name__ == "__main__":
    unittest.main()
