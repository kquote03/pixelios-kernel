"""board.json "xnu_bootargs_default" (oriole touch recipe, run oriole-gui7): added by tools/mkpayload.py, keys given on the cmdline win."""
import os, sys, unittest
sys.path.insert(0, os.path.dirname(__file__))
import mkpayload

GUI7 = ["sweettouchmux=1", "sweettouchcs=1", "sweettouchpoke=m:10c00640=10=0;d:100;m:10c0180c=f=5;", "sweettouchrstfirst=1"]
BASE = "debug=0xc serial=3 cpus=8 keepsyms=1 sweetusb=2"


class BoardArgs(unittest.TestCase):
    def test_oriole_gets_gui7_recipe(self):
        self.assertEqual(mkpayload.with_board_args(BASE, "oriole"), BASE + " " + " ".join(GUI7))

    def test_bluejay_and_sweet_unchanged(self):
        for b in ("bluejay", "sweet"):
            self.assertEqual(mkpayload.with_board_args(BASE, b), BASE)

    def test_cmdline_key_wins_no_duplicate(self):
        cl = BASE + " sweettouchrstfirst=0 sweettouchpoke=d:1;"
        out = mkpayload.with_board_args(cl, "oriole")
        self.assertEqual(out.split().count("sweettouchrstfirst=0"), 1)
        self.assertNotIn("sweettouchrstfirst=1", out)
        self.assertEqual(sum(1 for t in out.split() if t.startswith("sweettouchpoke=")), 1)
        self.assertIn("sweettouchmux=1", out)

    def test_flag_without_value_counts_as_key(self):
        out = mkpayload.with_board_args(BASE + " sweettouchcs", "oriole")
        self.assertNotIn("sweettouchcs=1", out)


if __name__ == "__main__":
    unittest.main()
