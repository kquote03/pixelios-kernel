#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Minimal read-only HFS+/HFSX file extractor (stdlib only).

Resolves HFS+ hard links (hlnk/hfs+ -> "[HFS+ Private Data]/iNode<N>") and the extents overflow
file, which 7-Zip 17 does not (it reports 'Data Error' for large fragmented files).

usage: hfs_extract.py IMAGE OUTDIR PATH [PATH...]   (PATH like /private/preboot/Cryptexes/OS/...;
       a trailing '*' in the last component extracts every matching name in that folder)
"""
import os
import struct
import sys

PRIV = "\0\0\0\0HFS+ Private Data"


class HFS:
    def __init__(self, path):
        self.f = open(path, "rb")
        vh = self.rd(1024, 512)
        if vh[:2] not in (b"H+", b"HX"):
            raise SystemExit("not HFS+/HFSX")
        self.bs = struct.unpack_from(">I", vh, 40)[0]
        self.hfsx = vh[:2] == b"HX"
        self.ext_fork = self.fork(vh, 192)
        self.cat_fork = self.fork(vh, 272)
        self.ext_fork = (self.ext_fork[0], self.ext_fork[1], 3)
        self.cat_fork = (self.cat_fork[0], self.cat_fork[1], 4)
        self.overflow = {}
        self.ext_data = self.read_fork(self.ext_fork, overflow=False)
        self.overflow = self.load_overflow()
        self.cat_data = self.read_fork(self.cat_fork, overflow=True)
        self.children = {}
        self.load_catalog()

    def rd(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    @staticmethod
    def fork(buf, off):
        size = struct.unpack_from(">Q", buf, off)[0]
        ext = [struct.unpack_from(">II", buf, off + 16 + 8 * i) for i in range(8)]
        return (size, [e for e in ext if e[1]])

    def read_fork(self, fork, overflow=True, fileid=None, out=None):
        size, ext = fork[0], list(fork[1])
        fid = fileid if fileid is not None else (fork[2] if len(fork) > 2 else None)
        if overflow and fid is not None:
            have = sum(c for _, c in ext)
            for start, blocks in self.overflow.get(fid, []):
                if start >= have:
                    ext.append(blocks)
        chunks = [] if out is None else None
        left = size
        for start, count in ext:
            if left <= 0:
                break
            n = min(left, count * self.bs)
            off = start * self.bs
            while n > 0:
                m = min(n, 64 << 20)
                b = self.rd(off, m)
                if out is None:
                    chunks.append(b)
                else:
                    out.write(b)
                off += m
                n -= m
                left -= m
        if left > 0:
            raise IOError("fork short by %d bytes (fileid %s)" % (left, fid))
        return b"".join(chunks) if out is None else None

    def leaves(self, data):
        hdr = data[14:]
        first = struct.unpack_from(">I", hdr, 10)[0]
        nsz = struct.unpack_from(">H", hdr, 18)[0]
        node = first
        while node:
            nd = data[node * nsz:(node + 1) * nsz]
            flink, _, kind, _, nrec = struct.unpack_from(">IIbBH", nd, 0)
            offs = [struct.unpack_from(">H", nd, nsz - 2 * (i + 1))[0] for i in range(nrec)]
            for i, o in enumerate(offs):
                end = offs[i + 1] if i + 1 < nrec else nsz - 2 * (nrec + 1)
                yield nd[o:end]
            node = flink

    def load_overflow(self):
        ov = {}
        if not self.ext_fork[0]:
            return ov
        for rec in self.leaves(self.ext_data):
            klen, ftype, _, fid, start = struct.unpack_from(">HBBII", rec, 0)
            if ftype != 0:
                continue
            body = rec[2 + klen:]
            ext = [struct.unpack_from(">II", body, 8 * i) for i in range(8)]
            ov.setdefault(fid, []).append((start, [e for e in ext if e[1]]))
        # flatten: list of (start_block_index, (startBlock, count)) per extent
        flat = {}
        for fid, recs in ov.items():
            for start, ext in sorted(recs):
                pos = start
                for e in ext:
                    flat.setdefault(fid, []).append((pos, e))
                    pos += e[1]
        return flat

    def load_catalog(self):
        for rec in self.leaves(self.cat_data):
            klen, parent, nlen = struct.unpack_from(">HIH", rec, 0)
            name = rec[8:8 + 2 * nlen].decode("utf-16-be", "replace")
            o = 2 + klen
            o += o & 1
            body = rec[o:]
            rtype = struct.unpack_from(">H", body, 0)[0]
            if rtype == 1:
                cnid = struct.unpack_from(">I", body, 8)[0]
                self.children.setdefault(parent, {})[name] = ("d", cnid, None)
            elif rtype == 2:
                cnid = struct.unpack_from(">I", body, 8)[0]
                special = struct.unpack_from(">I", body, 44)[0]
                ftype, creator = body[48:52], body[52:56]
                link = special if (ftype, creator) == (b"hlnk", b"hfs+") else None
                fork = self.fork(body, 88)
                self.children.setdefault(parent, {})[name] = ("f", cnid, (fork, link))

    def lookup(self, path):
        cur = ("d", 2, None)  # root folder's parent is 1; root cnid 2
        for comp in [c for c in path.split("/") if c]:
            ents = self.children.get(cur[1], {})
            if comp not in ents:
                return None
            cur = ents[comp]
        return cur

    def resolve(self, ent):
        fork, link = ent[2]
        if link is None:
            return ent[1], fork
        priv = self.children.get(2, {}).get(PRIV)
        inode = self.children.get(priv[1], {}).get("iNode%d" % link)
        return inode[1], inode[2][0]

    def extract(self, path, outdir):
        d, _, base = path.rpartition("/")
        names = [base]
        if base.endswith("*"):
            folder = self.lookup(d)
            names = sorted(n for n in self.children.get(folder[1], {}) if n.startswith(base[:-1]))
        for n in names:
            ent = self.lookup(d + "/" + n)
            if ent is None or ent[0] != "f":
                print("missing:", d + "/" + n)
                continue
            fid, fork = self.resolve(ent)
            dst = os.path.join(outdir, n)
            with open(dst, "wb") as o:
                self.read_fork(fork, fileid=fid, out=o)
            print("%s %d" % (dst, os.path.getsize(dst)))


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    h = HFS(sys.argv[1])
    os.makedirs(sys.argv[2], exist_ok=True)
    for p in sys.argv[3:]:
        h.extract(p, sys.argv[2])


if __name__ == "__main__":
    main()
