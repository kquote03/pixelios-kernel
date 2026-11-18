#!/usr/bin/env python3
"""Pack the stock AOC firmware (/vendor/firmware/aoc.bin) zlib-compressed into one container for xnu patch 0376 (sweetaoc=2, plan A6).

  pack_aocfw.py OUT --aoc FILE

Container: the WLANFW layout of tools/pack_wlanfw.py (pack_container/parse_container) with magic "AOCFWZB1" and one entry, tag "AOCF".
Entry data (little endian): u32 usize (uncompressed length); u32 flags = 1 (zlib); u8 sha256[32] of the uncompressed image; zlib stream.
Why compressed: the bootloader boots a 24.3 MB payload and refuses 32.7 MB (measured); aoc.bin is 15,158,976 B, ~3.3 MB after zlib -9.
The identity is pinned (docs/research/50 source `fw`): the packer refuses any other file, and so does the kernel (bsd/dev/sweet_aoc_load.h).
The blob stays outside git (lane rule: no firmware blobs in git).
"""
import argparse, hashlib, os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pack_wlanfw import pack_container, parse_container  # noqa: E402

MAGIC = b'AOCFWZB1'
TAG = b'AOCF'
SUB = 40                      # u32 usize, u32 flags, u8 sha256[32]
F_ZLIB = 1
AOC_LEN = 15158976            # docs/research/50 table, source `fw`
AOC_SHA = bytes.fromhex('9be5662290149cb014d89b630d2a19b36e98a6580c7898eb9b5874cc28985743')
MAX_Z = 7_500_000             # brief: above this the dev-overlay route is needed instead


def pack(image, pinned=True):
    if pinned and (len(image) != AOC_LEN or hashlib.sha256(image).digest() != AOC_SHA):
        raise ValueError(f'not the pinned aoc.bin ({len(image)} bytes, sha256 {hashlib.sha256(image).hexdigest()})')
    z = zlib.compress(image, 9)
    if len(z) > MAX_Z:
        raise ValueError(f'zlib stream {len(z)} bytes > {MAX_Z}')
    entry = struct.pack('<II32s', len(image), F_ZLIB, hashlib.sha256(image).digest()) + z
    return pack_container(MAGIC, (TAG,), [(TAG, entry)], 1)


def parse(buf, pinned=True):
    """container -> uncompressed image; raises ValueError on any structural, CRC, length or sha256 error."""
    (tag, entry), = parse_container(buf, MAGIC, (TAG,), 1)
    if len(entry) <= SUB:
        raise ValueError('entry too short')
    usize, flags, sha = struct.unpack_from('<II32s', entry, 0)
    if flags != F_ZLIB:
        raise ValueError(f'flags {flags}')
    d = zlib.decompressobj()
    image = d.decompress(entry[SUB:], usize + 1)
    if not d.eof or d.unconsumed_tail or d.unused_data or len(image) != usize or hashlib.sha256(image).digest() != sha:
        raise ValueError('zlib stream does not give usize bytes with the stated sha256')
    if pinned and (usize != AOC_LEN or sha != AOC_SHA):
        raise ValueError('not the pinned aoc.bin')
    return image


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--aoc', required=True)
    a = ap.parse_args()
    blob = pack(open(a.aoc, 'rb').read())
    open(a.out, 'wb').write(blob)
    parse(blob)
    elen = struct.unpack_from('<I', blob, 20)[0]
    print(f'container {len(blob)} bytes, entry AOCF {elen} bytes (zlib stream {elen - SUB}), crc32 {zlib.crc32(blob):#010x}')
    print('RESULT: PASS pack_aocfw')
