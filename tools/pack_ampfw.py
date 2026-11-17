#!/usr/bin/env python3
"""Pack the stock CS35L41 speaker-protection DSP files into one container for xnu patch 0169 (sweetaoc=6, plan A9e).

  pack_ampfw.py OUT --wmfw cs35l41-dsp1-spk-prot.wmfw --bin-l cs35l41-dsp1-spk-prot.bin --bin-r R-cs35l41-dsp1-spk-prot.bin

Source: vendor.img /vendor/firmware (docs/research/46 (a) 8); the R amp loads the "R-" tuning (DT sound-name-prefix "R" + cirrus,tuning-has-prefix,
AMP wm_adsp.c:2596-2602), both amps the same .wmfw (:1848-1849).
Container: the WLANFW layout of tools/pack_wlanfw.py (pack_container/parse_container) with magic "AMPFWV01" and three uncompressed entries in the
order WMFW, BINL, BINR (bsd/dev/sweet_amp_dsp.h amp_dsp_container reads exactly that). Every file is pinned by length + sha256 here and in the kernel.
~41 KB in total: no compression, far below the ~24 MB payload limit. The blob stays outside git (lane rule: no firmware blobs in git).
"""
import argparse, hashlib, os, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pack_wlanfw import pack_container, parse_container  # noqa: E402

MAGIC = b'AMPFWV01'
TAGS = (b'WMFW', b'BINL', b'BINR')
PINNED = {
    b'WMFW': (33988, 'cfee644db22850bb4a6ddae98144ba73c0756f98939d7bb5f905ae2b8468a525'),   # cs35l41-dsp1-spk-prot.wmfw
    b'BINL': (3884, '4b237795c876d5a3dc6708223525647821587acf67100449217b7bf229f36252'),    # cs35l41-dsp1-spk-prot.bin
    b'BINR': (3820, 'd6bc07fba542c1c68262c953f6a3070bf4561b1b76e6e1cbad1af86894c65154'),    # R-cs35l41-dsp1-spk-prot.bin
}


def check(tag, data):
    n, sha = PINNED[tag]
    if len(data) != n or hashlib.sha256(data).hexdigest() != sha:
        raise ValueError(f'{tag.decode()}: not the pinned file ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()})')


def pack(wmfw, bin_l, bin_r, pinned=True):
    blobs = [(b'WMFW', wmfw), (b'BINL', bin_l), (b'BINR', bin_r)]
    if pinned:
        for t, d in blobs:
            check(t, d)
    return pack_container(MAGIC, TAGS, blobs, 3)


def parse(buf, pinned=True):
    """container -> {tag: bytes}; raises ValueError on any structural, CRC, order, length or sha256 error."""
    res = parse_container(buf, MAGIC, TAGS, 3)
    if [t for t, _ in res] != list(TAGS):
        raise ValueError('entries not WMFW, BINL, BINR in this order')
    if pinned:
        for t, d in res:
            check(t, d)
    return dict(res)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--wmfw', required=True); ap.add_argument('--bin-l', required=True); ap.add_argument('--bin-r', required=True)
    a = ap.parse_args()
    blob = pack(*(open(p, 'rb').read() for p in (a.wmfw, a.bin_l, a.bin_r)))
    open(a.out, 'wb').write(blob)
    for t, d in parse(blob).items():
        print(f'{t.decode()} {len(d)} bytes sha256 {hashlib.sha256(d).hexdigest()[:16]}...')
    print(f'container {len(blob)} bytes, crc32 {zlib.crc32(blob):#010x}, sha256 {hashlib.sha256(blob).hexdigest()}')
    print('RESULT: PASS pack_ampfw')
