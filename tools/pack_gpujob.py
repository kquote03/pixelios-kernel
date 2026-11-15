#!/usr/bin/env python3
"""Pack Mali job captures (.malijob v1, docs/research/52 s.6) into one GPUJOBS1 container for the kernel replay (sweetmali bit 0x100,
xnu patch 0155).

  pack_gpujob.py OUT JOB.malijob [JOB.malijob ...]      (replay order = argument order, at most 4; names must be unique)

Container = the WLANFWB1 layout of tools/pack_wlanfw.py with magic "GPUJOBS1" and the single (repeatable) tag "GPUJ":
  0x00 char magic[8]; 0x08 u32 version = 1; 0x0c u32 count; 0x10 entry[count] { char tag[4]; u32 length; u32 offset; u32 crc32 (zlib) }
mkpayload.py --gpujob puts it in the payload as /chosen/memory-map GPUJOB <pa, size>. Each capture is checked with the same rules as the
kernel parser (bsd/dev/sweet_mali_replay.h smr_job_parse) before it is packed, so a file the kernel would refuse never reaches the phone.
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pack_wlanfw import pack_container, parse_container  # noqa: E402

MAGIC = b'GPUJOBS1'
TAG = b'GPUJ'
MAX_JOBS = 4
GPU_ID = 0x92020010                  # oriole, observed (SWEET GPU probe, PORT.md 2026-11-14)
VA_START, VA_END, PAGE = 0x02000000, 0x100000000, 0x4000
MAX_CHAINS, MAX_BOS, MAX_BO, MAX_JOB_BYTES, MAX_DIM = 8, 64, 32 << 20, 64 << 20, 4096
JOB_ALIGN, JOB_MIN = 128, 64
MAX_FILE = 16 << 20


def check_malijob(b, gpu_id=GPU_ID):
    """the rules of smr_job_parse; returns a summary dict or raises ValueError"""
    n = len(b)
    if n < 64 or n > MAX_FILE or struct.unpack_from('<Q', b, 32)[0] != n:
        raise ValueError('length / file_size')
    if b[:8] != b'PIXMJOB1':
        raise ValueError('magic')
    ver, hsz, gid, nch, nbo, flags = struct.unpack_from('<6I', b, 8)
    chain_off, rt_off, bo_off, data_off = struct.unpack_from('<4I', b, 40)
    if ver != 1 or hsz != 64 or flags != 1 or not 1 <= nch <= MAX_CHAINS or not 1 <= nbo <= MAX_BOS:
        raise ValueError('header')
    if gid != gpu_id:
        raise ValueError(f'gpu_id {gid:#x}')

    def inside(off, size):
        return 64 <= off <= n and size <= n - off
    if not (inside(chain_off, 16 * nch) and inside(rt_off, 64) and inside(bo_off, 64 * nbo) and inside(data_off, 0)):
        raise ValueError('table offsets')
    raw = b[56:64]
    name = raw.rstrip(b'\0')
    if not name or b'\0' in name or any(not (97 <= c <= 122 or 48 <= c <= 57 or c == 95) for c in name):
        raise ValueError('name')
    bos = []
    for i in range(nbo):
        va, size, doff, dlen, fl = struct.unpack_from('<4QI', b, bo_off + 64 * i)
        if (size == 0 or size > MAX_BO or size % PAGE or va % PAGE or not VA_START <= va < VA_END or size > VA_END - va or dlen > size
                or fl & ~7 or b[bo_off + 64 * i + 63] != 0 or doff < data_off or doff > n or dlen > n - doff):
            raise ValueError(f'bo {i}')
        label = b[bo_off + 64 * i + 40:bo_off + 64 * i + 64].split(b'\0', 1)[0]
        if any(c < 0x20 or c > 0x7e for c in label):
            raise ValueError(f'bo {i} label')   # review 0155 F2: printable ASCII only (the kernel prints it)
        for ova, osz in bos:
            if va < ova + osz and ova < va + size:
                raise ValueError(f'bo {i} overlaps')
        bos.append((va, size))
    total = sum(s for _, s in bos)
    if total > MAX_JOB_BYTES:
        raise ValueError('BOs too large')

    def in_bo(va, size):
        return any(bva <= va < bva + bsz and size <= bsz - (va - bva) for bva, bsz in bos)
    fs = False
    for i in range(nch):
        jc, req = struct.unpack_from('<QI', b, chain_off + 16 * i)
        if jc % JOB_ALIGN or req & ~1 or not in_bo(jc, JOB_MIN):
            raise ValueError(f'chain {i}')
        fs |= bool(req & 1)
    if not fs:
        raise ValueError('no fragment chain')
    rva, w, h, stride, fmt, crc, crc_valid = struct.unpack_from('<Q6I', b, rt_off)
    if fmt != 1 or not 1 <= w <= MAX_DIM or not 1 <= h <= MAX_DIM or stride < 4 * w or crc_valid > 1 or not in_bo(rva, stride * h):
        raise ValueError('render target')
    for i in range(3):
        x, y, _ = struct.unpack_from('<HHI', b, rt_off + 32 + 8 * i)
        if x >= w or y >= h:
            raise ValueError(f'pixel {i}')
    return dict(name=name.decode(), n_chains=nch, n_bos=nbo, bo_bytes=total, rt=(rva, w, h, stride), crc_valid=crc_valid, expect_crc=crc)


def pack(jobs):
    names = [check_malijob(j)['name'] for j in jobs]
    if len(set(names)) != len(names):
        raise ValueError(f'job names not unique: {names}')
    return pack_container(MAGIC, (TAG,), [(TAG, j) for j in jobs], MAX_JOBS, unique=False)


def parse(buf):
    """container -> [malijob bytes]; each one passes check_malijob"""
    if len(buf) > MAX_FILE:
        raise ValueError('container too large')
    jobs = [d for _, d in parse_container(buf, MAGIC, (TAG,), MAX_JOBS, unique=False)]
    for j in jobs:
        check_malijob(j)
    return jobs


if __name__ == '__main__':
    import zlib
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    blob = pack([open(p, 'rb').read() for p in sys.argv[2:]])
    open(sys.argv[1], 'wb').write(blob)
    for j in parse(blob):
        i = check_malijob(j)
        print(f"{i['name']}: {len(j)} bytes crc32 {zlib.crc32(j):#010x} chains {i['n_chains']} bos {i['n_bos']} wired {i['bo_bytes']} "
              f"rt {i['rt'][0]:#x} {i['rt'][1]}x{i['rt'][2]} crc_valid {i['crc_valid']} expect {i['expect_crc']:#010x}")
    print(f'container {len(blob)} bytes, crc32 {zlib.crc32(blob):#010x}')
    print('RESULT: PASS pack_gpujob')
