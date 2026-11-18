"""tools/pack_aocfw.py: AOCFWZB1 container of the stock aoc.bin (xnu patch 0376, sweetaoc=2), mkpayload --aocfw, build-xnu-image.sh/xrun --aocfw.
The layout must match what bsd/dev/sweet_aoc_load.h (swal_container) reads; the pinned identity must be the one of docs/research/50."""
import os
import re
import struct
import sys
import unittest
import zlib

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import pack_aocfw as p  # noqa: E402

ROOT = os.path.dirname(here)


class PackAocfw(unittest.TestCase):
    img = bytes((i * 7 + (i >> 9)) & 0xFF for i in range(70000))

    def test_round_trip_and_layout(self):
        c = p.pack(self.img, pinned=False)
        self.assertEqual(c[:8], b'AOCFWZB1')
        version, count, tag, elen, eoff, crc = struct.unpack_from('<II4sIII', c, 8)
        self.assertEqual((version, count, tag, eoff), (1, 1, b'AOCF', 32))
        self.assertEqual(crc, zlib.crc32(c[32:32 + elen]))
        self.assertLessEqual(32 + elen, len(c))
        usize, flags = struct.unpack_from('<II', c, 32)
        self.assertEqual((usize, flags), (len(self.img), 1))
        self.assertEqual(p.parse(c, pinned=False), self.img)

    def test_refuses(self):
        with self.assertRaises(ValueError):
            p.pack(self.img)                            # not the pinned image
        c = bytearray(p.pack(self.img, pinned=False))
        with self.assertRaises(ValueError):
            p.parse(bytes(c))                           # pinned check on parse too
        c[100] ^= 1
        with self.assertRaises(ValueError):
            p.parse(bytes(c), pinned=False)             # crc

    def test_pinned_identity_matches_note_and_kernel(self):
        note = open(os.path.join(ROOT, 'docs', 'research', '50-trusty-gsa-aoc-transport.md')).read()
        self.assertIn(p.AOC_SHA.hex(), note)
        self.assertIn('15,158,976', note)
        self.assertEqual(p.AOC_LEN, 15158976)
        patch = open(os.path.join(ROOT, 'xnu', 'patches', '0376-aoc-load.patch')).read()
        self.assertIn('#define SWAL_FW_LEN       15158976u', patch)
        m = re.search(r'swal_fw_sha\[32\] = \{(.*?)\};', patch, re.S)
        got = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m.group(1).replace('\n+', '\n')))
        self.assertEqual(got, p.AOC_SHA)

    def test_flags_wired(self):
        self.assertIn('--aocfw', open(os.path.join(here, 'xrun')).read())
        self.assertIn('--aocfw', open(os.path.join(here, 'build-xnu-image.sh')).read())
        src = open(os.path.join(here, 'mkpayload.py')).read()
        self.assertIn("'--aocfw'", src)
        self.assertIn("'AOCFW'", src)


if __name__ == '__main__':
    unittest.main()
