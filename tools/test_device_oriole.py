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

    def test_batt_gauge_publish_reads_only(self):
        """Patch 0395 (battery publish on upstream's charger engine): fuel-gauge reads only, the allowlisted registers, plausibility bounds, never 0 % before a sample."""
        import glob
        import re
        p = glob.glob(os.path.join(ROOT, "xnu", "patches", "0395-*.patch"))
        self.assertEqual(len(p), 1)
        with open(p[0]) as f:
            added = "\n".join(l[1:] for l in f.read().splitlines() if l.startswith("+") and not l.startswith("+++"))
        self.assertEqual(sorted(re.findall(r"chg_i2c_read\((0x[0-9a-f]+), (0x[0-9a-f]+)", added)), [("0x36", "0x06"), ("0x36", "0x09")])
        self.assertEqual(re.findall(r"i2c_wr\(([^)]*)\)", added), ["I_INTEN, 0", "I_INTEN, 0"])  # the per-transfer interrupt-enable clear of sweet_chg_fg_sample, nothing else
        for bad in ("i2c_cnfg09_write", "i2c_hw_wr32", "ml_io_map", "PE_parse_boot_arg"):
            self.assertNotIn(bad, added)
        self.assertIn("mv < 2800u || mv > 4600u || soc > 10200u", added)
        self.assertIn("publish(100, 4200, 0, true, true, false, true, false, false);", added)  # static fallback before the first set
        self.assertIn("* 78125u) / 1000000u", added)   # VCell 78.125 uV/LSB, as mon_row
        self.assertIn("* 100u) / 256u", added)         # RepSOC 1/256 %, in centi-%

    def test_is_a_phone_target(self):
        self.assertEqual(boards.target_board({"BOARD": "oriole"}), "oriole")

    def test_pix_check(self):
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "pix"), "check", "oriole"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("oriole: ok", r.stdout)


if __name__ == "__main__":
    unittest.main()
