"""tools/pack_ampfw.py: AMPFWV01 container of the CS35L41 DSP files (xnu patch 0381, sweetaoc=6), mkpayload / build-xnu-image.sh / xrun --ampfw.
The layout must match bsd/dev/sweet_amp_dsp.h amp_dsp_container; the pinned identity must be the kernel's."""
import hashlib
import os
import re
import struct
import sys
import unittest

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import pack_ampfw as p  # noqa: E402

ROOT = os.path.dirname(here)


class PackAmpfw(unittest.TestCase):
    w, l, r = b'W' * 40, b'L' * 13, b'R' * 9

    def test_layout_matches_kernel_reader(self):
        c = p.pack(self.w, self.l, self.r, pinned=False)
        self.assertEqual(c[:8], b'AMPFWV01')
        self.assertEqual(struct.unpack_from('<II', c, 8), (1, 3))
        for i, (tag, data) in enumerate(((b'WMFW', self.w), (b'BINL', self.l), (b'BINR', self.r))):
            t, n, off, _ = struct.unpack_from('<4sIII', c, 16 + 16 * i)
            self.assertEqual((t, n), (tag, len(data)))
            self.assertEqual(off & 7, 0)
            self.assertEqual(c[off:off + n], data)
        self.assertEqual(p.parse(c, pinned=False)[b'BINR'], self.r)

    def test_refuses(self):
        with self.assertRaises(ValueError):
            p.pack(self.w, self.l, self.r)                       # not the pinned files
        c = bytearray(p.pack(self.w, self.l, self.r, pinned=False))
        with self.assertRaises(ValueError):
            p.parse(bytes(c))                                    # pinned check on parse
        c[70] ^= 1
        with self.assertRaises(ValueError):
            p.parse(bytes(c), pinned=False)                      # crc

    def test_pinned_identity_matches_kernel(self):
        patch = open(os.path.join(ROOT, 'xnu', 'patches', '0381-amp-dsp-load.patch')).read()
        m = re.search(r'amp_fw_len\[AMP_FW_N\] = \{ (\d+)u, (\d+)u, (\d+)u \}', patch)
        self.assertEqual([int(x) for x in m.groups()], [p.PINNED[t][0] for t in p.TAGS])
        m = re.search(r'amp_fw_sha\[AMP_FW_N\]\[32\] = \{(.*?)\n\+\};', patch, re.S)
        got = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m.group(1)))
        self.assertEqual(got.hex(), ''.join(p.PINNED[t][1] for t in p.TAGS))

    def test_stock_files_when_present(self):
        d = os.environ.get('SWEET_CS35L41_FW') or os.path.expanduser('~/pixelios-artifacts/aoc/cs35l41')
        names = ('cs35l41-dsp1-spk-prot.wmfw', 'cs35l41-dsp1-spk-prot.bin', 'R-cs35l41-dsp1-spk-prot.bin')
        if not all(os.path.exists(os.path.join(d, n)) for n in names):
            self.skipTest('stock files not present')
        blobs = [open(os.path.join(d, n), 'rb').read() for n in names]
        c = p.pack(*blobs)
        self.assertEqual(p.parse(c)[b'WMFW'], blobs[0])
        self.assertEqual(hashlib.sha256(blobs[1]).hexdigest(), p.PINNED[b'BINL'][1])

    def test_flags_wired(self):
        self.assertIn('--ampfw', open(os.path.join(here, 'xrun')).read())
        self.assertIn('--ampfw', open(os.path.join(here, 'build-xnu-image.sh')).read())
        src = open(os.path.join(here, 'mkpayload.py')).read()
        self.assertIn("'--ampfw'", src)
        self.assertIn("'AMPFW'", src)


if __name__ == '__main__':
    unittest.main()
