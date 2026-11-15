"""tools/pack_gpujob.py: GPUJOBS1 container of .malijob captures (docs/research/52), mkpayload --gpujob, build-xnu-image.sh --gpujob."""
import os, struct, subprocess, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(__file__))
import pack_gpujob as g
import mkpayload
from tools import adt

GPU = 0x92020010


def job(name=b'tri', chains=((0x03054300, 0), (0x03054680, 1)), rt_va=0x03100000):
    """a minimal valid .malijob v1: heap (no data), descriptor BO with 0x40 bytes of data, RT BO"""
    bos = [(0x02000000, 0x1000000, b'', 6), (0x03054000, 0x10000, bytes(range(64)), 0), (0x03100000, 0x4000, b'', 0)]
    chain_off, rt_off, bo_off = 64, 192, 256
    data_off = bo_off + 64 * len(bos)
    out = bytearray(data_off)
    struct.pack_into('<8s6I', out, 0, b'PIXMJOB1', 1, 64, GPU, len(chains), len(bos), 1)
    struct.pack_into('<4I8s', out, 40, chain_off, rt_off, bo_off, data_off, name)
    for i, (jc, req) in enumerate(chains):
        struct.pack_into('<QII', out, chain_off + 16 * i, jc, req, 0)
    struct.pack_into('<Q6I', out, rt_off, rt_va, 64, 32, 256, 1, 0, 0)
    for i, (x, y, c) in enumerate(((0, 0, 0xff996633), (63, 31, 0xff996633), (32, 16, 0xff00ff00))):
        struct.pack_into('<HHI', out, rt_off + 32 + 8 * i, x, y, c)
    off = data_off
    for i, (va, size, data, fl) in enumerate(bos):
        struct.pack_into('<4QII24s', out, bo_off + 64 * i, va, size, off, len(data), fl, 0, b'bo%d' % i)
        off += len(data)
    for _, _, data, _ in bos:
        out += data
    struct.pack_into('<Q', out, 32, len(out))
    return bytes(out)


class Malijob(unittest.TestCase):
    def test_valid(self):
        info = g.check_malijob(job())
        self.assertEqual((info['name'], info['n_chains'], info['n_bos']), ('tri', 2, 3))
        self.assertEqual(info['bo_bytes'], 0x1000000 + 0x10000 + 0x4000)

    def test_refused(self):
        j = job()
        bad = [j[:-1], j + b'\0', b'X' + j[1:]]
        def put(off, fmt, v):
            d = bytearray(j); struct.pack_into(fmt, d, off, v); return bytes(d)
        bad += [put(8, '<I', 2), put(16, '<I', 0x72120000), put(20, '<I', 0), put(20, '<I', 9), put(24, '<I', 65), put(28, '<I', 0),
                put(56, '<8s', b'TRI'), put(56, '<8s', b''),
                put(64, '<Q', 0x03054340), put(64, '<Q', 0x03010000), put(72, '<I', 2), put(88, '<I', 0),
                put(192, '<Q', 0x03104000), put(192 + 16, '<I', 255), put(192 + 20, '<I', 2), put(192 + 32, '<H', 64),
                put(256, '<Q', 0x01ffc000), put(256 + 64, '<Q', 0x02ffc000), put(256 + 128 + 8, '<Q', 0x5000), put(256 + 64 + 24, '<Q', 0x10001),
                put(256 + 32, '<I', 8),
                put(256 + 40, '<4s', b'b\nX\0'), put(256 + 40, '<4s', b'b\x7f\0\0'), put(256 + 40, '<4s', b'b\xc3\0\0')]  # review 0155 F2
        for i, b in enumerate(bad):
            with self.assertRaises(ValueError, msg=f'case {i}'):
                g.check_malijob(b)

    def test_name_from_file_must_match_order(self):
        with self.assertRaises(ValueError):
            g.pack([job(b'tri'), job(b'tri')])          # names must be unique: the replay prints them


class Container(unittest.TestCase):
    def test_roundtrip(self):
        c = g.pack([job(b'clear', chains=((0x03054680, 1),)), job(b'tri')])
        jobs = g.parse(c)
        self.assertEqual([g.check_malijob(x)['name'] for x in jobs], ['clear', 'tri'])
        self.assertEqual(c[:8], b'GPUJOBS1')
        self.assertEqual(len(c) % 8, 0)

    def test_crc_and_limits(self):
        c = bytearray(g.pack([job()]))
        off, = struct.unpack_from('<I', c, 16 + 8)
        c[off + 70] ^= 1
        with self.assertRaises(ValueError):
            g.parse(bytes(c))
        with self.assertRaises(ValueError):
            g.pack([])
        with self.assertRaises(ValueError):
            g.pack([job(b'a%d' % i) for i in range(5)])
        with self.assertRaises(ValueError):
            g.parse(b'WLANFWB1' + bytes(c[8:]))


def tiny_adt():
    def node(name, children=()):
        n = adt.Node(); n.props.append(('name', 0, name.encode() + b'\0')); n.children.extend(children); return n
    return adt.serialize(node('device-tree', [node('chosen', [node('memory-map')])]))


class Payload(unittest.TestCase):
    def test_gpujob_property(self):
        blob = mkpayload.add_ramdisk_to_adt(tiny_adt(), 0x881000000, 0x2000, 'GPUJOB')
        root, _ = adt.parse(blob)
        self.assertEqual(root.children[0].children[0].prop('GPUJOB'), struct.pack('<2Q', 0x881000000, 0x2000))

    def test_build_script_flag(self):
        here = os.path.dirname(os.path.abspath(__file__))
        src = open(os.path.join(here, 'build-xnu-image.sh')).read()
        self.assertIn('--gpujob', src)
        self.assertIn('--gpujob', open(os.path.join(here, 'xrun')).read())
        self.assertIn("'--gpujob'", open(os.path.join(here, 'mkpayload.py')).read())


if __name__ == '__main__':
    unittest.main()
