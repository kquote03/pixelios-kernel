"""device/google/oriole (Pixel 6): same SoC and same reference-board wiring as bluejay, so the shim header must be identical."""
import os
import subprocess
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import boards  # noqa: E402

# Keys whose oriole values are read from the same Google gs101 reference files as bluejay's (device/google/oriole/board.json notes).
SHARED = ("fb_base", "fb_size", "fb_width", "fb_height", "fb_stride", "key_pins", "key_active_low", "touch_chip_id", "touch_usi",
          "touch_max_hz", "touch_max_coords", "touch_spi_pin_func", "touch_pin_clk", "touch_pin_mosi", "touch_pin_miso",
          "touch_pin_cs", "touch_pin_irq", "touch_pin_reset", "touch_pin_mux")


class Oriole(unittest.TestCase):
    def test_listed_on_gs101(self):
        self.assertIn("oriole", boards.BOARDS)
        self.assertEqual(boards._DEVICES["oriole"][2]["soc"], "google/gs101")
        self.assertEqual(boards.device_name("oriole"), "Pixel 6")

    def test_shim_header_identical_to_bluejay(self):
        # upstream 2026-11-15 added EL2_DRAM_BANKS (from board.json dram_banks): the only line allowed to differ, because oriole has 8 GiB
        # (high banks 0x880000000 + 3 x 2 GiB, observed oriole-probe1) and bluejay 6 GiB; every other line stays identical
        o = [l for l in boards.c_header("oriole").splitlines() if not l.startswith("#define EL2_DRAM_BANKS ")]
        b = [l for l in boards.c_header("bluejay").splitlines() if not l.startswith("#define EL2_DRAM_BANKS ")]
        self.assertEqual(o, b)
        self.assertIn("#define EL2_DRAM_BANKS { 0x80000000UL, 0x80000000UL }, { 0x880000000UL, 0x180000000UL }", boards.c_header("oriole"))

    def test_reference_board_values_match_bluejay(self):
        for k in SHARED:
            self.assertEqual(boards.BOARDS["oriole"][k], boards.BOARDS["bluejay"][k], k)

    def test_xnu_ram_inside_one_dram_bank(self):
        base, size = boards.BOARDS["oriole"]["xnu_ram"]
        self.assertTrue(any(b <= base and base + size <= b + s for b, s in boards.BOARDS["oriole"]["dram_banks"]))

    def test_is_a_phone_target(self):
        self.assertEqual(boards.target_board({"BOARD": "oriole"}), "oriole")

    def test_pix_check(self):
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "pix"), "check", "oriole"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("oriole: ok", r.stdout)


if __name__ == "__main__":
    unittest.main()
