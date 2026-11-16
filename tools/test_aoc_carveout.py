"""A2 follow-up (docs/research/50 section 8): the AOC DRAM carveout of oriole lies inside physical DRAM but entirely OUTSIDE the RAM handed to XNU,
both in board.json (aoc_carveout vs xnu_ram) and in the ADT that tools/mkadt.py really builds (/chosen dram-base / dram-size). The AOC firmware and the
GSA own that range (gsa_aoc.h:26-41: the body is locked after LOAD_AOC_FW_IMG), so XNU's page allocator must never hand it out."""
import os
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import adt, boards, mkadt  # noqa: E402

# typed here from the observed stock capture (oriole-ref1 dmesg:21 "aoc@94000000 ... 49152 KiB", iomem.txt:186-187 "94000000-96ffffff : 19000000.aoc"),
# never from board.json
AOC_BASE, AOC_SIZE = 0x94000000, 0x03000000


def overlaps(a, an, b, bn):
    return a < b + bn and b < a + an


class AocCarveout(unittest.TestCase):
    def setUp(self):
        self.cfg = boards.BOARDS["oriole"]

    def test_board_json_carveout_is_the_observed_one(self):
        base, size = self.cfg["aoc_carveout"]
        self.assertEqual((base, size), (AOC_BASE, AOC_SIZE))

    def test_carveout_inside_a_dram_bank(self):
        self.assertTrue(any(b <= AOC_BASE and AOC_BASE + AOC_SIZE <= b + s for b, s in self.cfg["dram_banks"]))

    def test_carveout_outside_xnu_ram(self):
        base, size = self.cfg["xnu_ram"]
        self.assertFalse(overlaps(AOC_BASE, AOC_SIZE, base, size), f"xnu_ram {base:#x}+{size:#x} overlaps the AOC carveout")

    def test_carveout_outside_the_adt_dram(self):
        root = adt.parse(adt.serialize(mkadt.build(seed=b"\0" * 64, board="oriole")))[0]
        chosen = next(c for c in root.children if dict((k, v) for k, _f, v in c.props).get("name") == b"chosen\0")
        p = dict((k, v) for k, _f, v in chosen.props)
        base, size = struct.unpack("<Q", p["dram-base"])[0], struct.unpack("<Q", p["dram-size"])[0]
        self.assertEqual((base, size), tuple(self.cfg["xnu_ram"]))
        self.assertFalse(overlaps(AOC_BASE, AOC_SIZE, base, size), f"ADT dram {base:#x}+{size:#x} overlaps the AOC carveout")

    def test_overlap_helper(self):	# the check itself must be able to fail
        self.assertTrue(overlaps(AOC_BASE, AOC_SIZE, 0x80000000, 0x80000000))
        self.assertTrue(overlaps(AOC_BASE, AOC_SIZE, 0x96FFF000, 0x1000))
        self.assertFalse(overlaps(AOC_BASE, AOC_SIZE, 0x97000000, 0x1000))
        self.assertFalse(overlaps(AOC_BASE, AOC_SIZE, 0x880000000, 0x100000000))


if __name__ == "__main__":
    unittest.main()
