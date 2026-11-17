#!/usr/bin/env python3
"""Pack a directory into a SWEETDEV archive for the dev overlay (xnu patch 0117, bsd/dev/sweet_devoverlay.c).

  tools/mkdevoverlay.py DIR OUT.bin [--owner UID:GID] [--zlib-above BYTES]

File DIR/usr/lib/x becomes /usr/lib/x on the phone. Only paths the kernel allowlist accepts are packed (anything
else is an error, not a silent skip); symlinks are refused; total data is capped at 40 MiB. Output is deterministic
(sorted paths, mode = file mode & 0777, one owner, no timestamps). Format (little-endian), the kernel validates it all:
  header: "SWEETDEV" | u32 version=1 | u32 count
  entry (16-byte aligned): u16 pathlen | u16 mode | u32 uid | u32 gid | u32 crc32(data) | u64 size
                           | path | pad16 | data | pad16
  version 2 (xnu patch 0163; written only when --zlib-above compresses some file, so archives without it are
  byte-identical to before): entry header is 40 bytes, the 24 above with crc32 of the UNCOMPRESSED data and size =
  stored length, then u64 usize | u32 flags (0 stored, 1 zlib) | u32 0; usize <= 40 MiB per entry and in total.
  A zlib entry is valid only if the stream ends exactly at the end of the stored bytes, inflates to exactly usize
  bytes and matches the crc (the kernel checks this before the rename).
parse() / path_check() below are the Python twins of the kernel checks (used by the tests).
"""
import argparse, os, stat, struct, sys, zlib

MAGIC, VERSION, HDR, ENT = b'SWEETDEV', 1, 16, 24
VERSION2, ENT2, F_ZLIB = 2, 40, 1
MAX_COUNT, MAX_PATH, MAX_TOTAL = 4096, 1000, 40 << 20   # 40 MiB: the AVB boot image is 64 MiB and holds ~16 MiB already
# (prefix, kind): D = directory prefix, X = exact path, N = file name prefix in one directory
ALLOW = [('/usr/lib/', 'D'), ('/usr/local/', 'D'), ('/private/var/dev/', 'D'),
         ('/System/Library/xpc/launchd.plist', 'X'), ('/System/Library/LaunchDaemons/org.pixelios.', 'N')]


def path_check(p):
    """None if the kernel accepts this path, else the reason (same rules as sd_path_check in the kernel)."""
    b = p.encode('utf-8', 'surrogateescape') if isinstance(p, str) else p
    if not b or b[0:1] != b'/':
        return 'not absolute'
    if any(c < 0x20 or c >= 0x7f for c in b):
        return 'control or non-ASCII byte in path'
    for comp in b[1:].split(b'/'):
        if comp == b'':
            return 'empty component'
        if comp == b'.':
            return "'.' component"
        if comp == b'..':
            return "'..' component"
        if len(comp) > 245:
            return 'component longer than 245 bytes'
    if b.lower().endswith((b'.sweettmp', b'.sweetorig')):
        return 'reserved suffix'
    for pfx, kind in ALLOW:
        pb = pfx.encode()
        if not b.startswith(pb):
            continue
        rest = b[len(pb):]
        if (kind == 'X' and rest) or (kind == 'N' and (not rest or b'/' in rest)) or (kind == 'D' and not rest):
            continue
        return None
    return 'not in allowlist'


def _pad(n):
    return (n + 15) & ~15


def pack(entries, zlib_above=None):
    """entries: list of (path:str, mode, uid, gid, data) -> archive bytes. zlib_above: compress (zlib level 9) every file
    larger than this many bytes when that makes it smaller; if no file ends up compressed the archive is version 1,
    byte-identical to what this function wrote before compression existed."""
    if len(entries) > MAX_COUNT:
        raise ValueError('too many files (%d > %d)' % (len(entries), MAX_COUNT))
    stored = []
    for path, mode, uid, gid, data in entries:
        z = zlib.compress(data, 9) if zlib_above is not None and len(data) > zlib_above else None
        stored.append((z, F_ZLIB) if z is not None and len(z) < len(data) else (data, 0))
    v2 = any(f for _, f in stored)
    if v2 and sum(len(e[4]) for e in entries) > MAX_TOTAL:
        raise ValueError('more than 40 MiB uncompressed')
    out = bytearray(MAGIC + struct.pack('<II', VERSION2 if v2 else VERSION, len(entries)))
    for (path, mode, uid, gid, data), (body, flags) in zip(entries, stored):
        pb = path.encode('utf-8', 'surrogateescape')
        if len(pb) > MAX_PATH:
            raise ValueError('path too long: ' + path)
        out += struct.pack('<HHIIIQ', len(pb), mode, uid, gid, zlib.crc32(data) & 0xffffffff, len(body))
        if v2:
            out += struct.pack('<QII', len(data), flags, 0)
        out += pb
        out += b'\0' * (_pad(len(out)) - len(out)) + body
        out += b'\0' * (_pad(len(out)) - len(out))
    return bytes(out)


def _inflate(body, usize):
    """Twin of the kernel's sd_stream for a zlib entry: (data, ok)."""
    d = zlib.decompressobj()
    try:
        out = d.decompress(body, usize + 1)               # never more than usize + 1 bytes: a bomb stops here
    except zlib.error:
        return b'', False
    return out, d.eof and not d.unused_data and not d.unconsumed_tail and len(out) == usize


def parse(buf, with_usize=False):
    """Validate exactly like the kernel; returns [(path, mode, uid, gid, data, crc_ok)] (+ usize if with_usize) or raises
    ValueError (a bad crc or zlib stream only flags that entry, as in the kernel). data is the uncompressed bytes."""
    n = len(buf)
    if n < HDR or buf[:8] != MAGIC:
        raise ValueError('bad magic')
    ver, count = struct.unpack_from('<II', buf, 8)
    if ver not in (VERSION, VERSION2):
        raise ValueError('bad version')
    hl = ENT2 if ver == VERSION2 else ENT
    if count > MAX_COUNT:
        raise ValueError('bad count')
    off, ents, total = HDR, [], 0
    for i in range(count):
        if off > n or n - off < hl:
            raise ValueError('truncated entry header')
        plen, mode, uid, gid, crc, size = struct.unpack_from('<HHIIIQ', buf, off)
        usize, flags = size, 0
        if ver == VERSION2:
            usize, flags, zero = struct.unpack_from('<QII', buf, off + ENT)
            if zero or flags > F_ZLIB or (flags == 0 and usize != size) or usize > MAX_TOTAL:
                raise ValueError('bad version 2 entry fields')
        total += usize
        off += hl
        if plen == 0 or plen > MAX_PATH or plen > n - off:
            raise ValueError('bad pathlen')
        path = bytes(buf[off:off + plen])
        off = _pad(off + plen)
        if off > n or size > n - off:
            raise ValueError('bad size')
        data = bytes(buf[off:off + size])
        off = _pad(off + size)
        if off > n:
            raise ValueError('truncated padding')
        ok = True
        if flags:
            data, ok = _inflate(data, usize)
        e = (path.decode('utf-8', 'surrogateescape'), mode, uid, gid, data, ok and zlib.crc32(data) & 0xffffffff == crc)
        ents.append(e + (usize,) if with_usize else e)
    if ver == VERSION2 and total > MAX_TOTAL:
        raise ValueError('more than 40 MiB uncompressed')
    return ents


def collect(root, uid=0, gid=0):
    entries, total = [], 0
    seen = {}
    for d, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        for name in sorted(dirs + files):
            full = os.path.join(d, name)
            if os.path.islink(full):
                sys.exit('mkdevoverlay: refusing symlink %s' % full)
        for name in sorted(files):
            full = os.path.join(d, name)
            st = os.lstat(full)                            # never open a FIFO/socket/device (open would block)
            if not stat.S_ISREG(st.st_mode):
                sys.exit('mkdevoverlay: not a regular file %s' % full)
            path = '/' + os.path.relpath(full, root).replace(os.sep, '/')
            why = path_check(path)
            if why:
                sys.exit('mkdevoverlay: %s is not allowed on the phone (%s); allowed: %s'
                         % (path, why, ', '.join(p for p, _ in ALLOW)))
            if path.lower() in seen:                       # the root may be case-insensitive HFS+: both would be one file (review 19 L2)
                sys.exit('mkdevoverlay: %s and %s differ only by case' % (seen[path.lower()], path))
            seen[path.lower()] = path
            data = open(full, 'rb').read()
            total += len(data)
            if total > MAX_TOTAL:
                sys.exit('mkdevoverlay: more than 40 MiB of files')
            if uid != 0 and (path.startswith('/usr/lib/') or path.startswith('/System/')):
                print('mkdevoverlay: WARNING %s will be owned by uid %d, not root' % (path, uid), file=sys.stderr)
            entries.append((path, st.st_mode & 0o777, uid, gid, data))
    entries.sort(key=lambda e: e[0])
    return entries


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dir'); ap.add_argument('out')
    ap.add_argument('--owner', default='0:0', help='UID:GID of every file (default 0:0)')
    ap.add_argument('--zlib-above', type=int, metavar='BYTES',
                    help='zlib-compress files larger than BYTES (needs kernel patch 0163; off: version 1 archive as before)')
    a = ap.parse_args()
    try:
        uid, gid = (int(x) for x in a.owner.split(':'))
        assert 0 <= uid < 2**32 and 0 <= gid < 2**32
    except (ValueError, AssertionError):
        sys.exit('mkdevoverlay: --owner needs UID:GID')
    if not os.path.isdir(a.dir) or os.path.islink(a.dir):
        sys.exit('mkdevoverlay: %s is not a directory' % a.dir)
    entries = collect(a.dir, uid, gid)
    if not entries:
        sys.exit('mkdevoverlay: no files in %s' % a.dir)
    try:
        blob = pack(entries, a.zlib_above)
        parse(blob)                                        # the packer's own output must pass the kernel checks
    except ValueError as e:
        sys.exit('mkdevoverlay: %s' % e)
    open(a.out, 'wb').write(blob)
    print('mkdevoverlay: %d files, %d bytes (version %d) -> %s' % (len(entries), len(blob), struct.unpack_from('<I', blob, 8)[0], a.out))


if __name__ == '__main__':
    main()
