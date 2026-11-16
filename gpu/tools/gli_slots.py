#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recover Apple's GLI dispatch slot map from a (split) arm64 dyld shared cache (stdlib only).

GLEngine's _gliInitDispatchTable(disp) stores one function pointer per slot at disp+8*slot. This tool
emulates the straight-line ADRP/ADR/ADD/MOV/STR/STP/STUR stores of that function (register tracking,
no branches followed), maps each stored pointer to its GLEngine symbol and prints "<slot> <glName>".

usage: gli_slots.py CACHE_DIR [--image SUBSTR] [--func NAME] [--raw] [--dump]
  --raw   print "<slot> <symbol>" without converting symbol -> gl entry name
  --dump  print every decoded store (debug)
"""
import bisect
import glob
import os
import re
import struct
import sys


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Cache:
    def __init__(self, d):
        files = sorted(glob.glob(os.path.join(d, "dyld_shared_cache_arm64*")))
        self.main_path = os.path.join(d, "dyld_shared_cache_arm64")
        self.maps = []  # (vmaddr, size, fd, fileoff)
        self.fds = {}
        for p in files:
            if p.endswith(".symbols"):
                continue
            fd = open(p, "rb")
            hdr = fd.read(0x200)
            if not hdr.startswith(b"dyld_v1"):
                continue
            moff, mcnt = u32(hdr, 16), u32(hdr, 20)
            fd.seek(moff)
            mb = fd.read(32 * mcnt)
            for i in range(mcnt):
                a, s, fo = struct.unpack_from("<QQQ", mb, 32 * i)
                self.maps.append((a, s, fd, fo))
            self.fds[p] = fd
        self.maps.sort(key=lambda m: m[0])
        self.starts = [m[0] for m in self.maps]
        mf = self.fds[self.main_path]
        mf.seek(0)
        self.hdr = mf.read(0x200)
        self.base = u64(self.hdr, 224)  # sharedRegionStart

    def read(self, addr, n):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            raise KeyError(hex(addr))
        a, s, fd, fo = self.maps[i]
        if addr + n > a + s:
            raise KeyError(hex(addr))
        fd.seek(fo + addr - a)
        return fd.read(n)

    def images(self):
        h = self.hdr
        moff = u32(h, 16)
        if moff > 0x1c8:
            off, cnt = u32(h, 0x1c0), u32(h, 0x1c4)
        else:
            off, cnt = u32(h, 24), u32(h, 28)
        mf = self.fds[self.main_path]
        mf.seek(off)
        tab = mf.read(32 * cnt)
        out = []
        for i in range(cnt):
            addr, _, _, poff = struct.unpack_from("<QQQI", tab, 32 * i)
            mf.seek(poff)
            out.append((addr, mf.read(1024).split(b"\0", 1)[0].decode()))
        return out


def macho(cache, addr):
    h = cache.read(addr, 32)
    ncmds, sizeofcmds = u32(h, 16), u32(h, 20)
    lc = cache.read(addr + 32, sizeofcmds)
    segs, symtab, o = {}, None, 0
    for _ in range(ncmds):
        cmd, sz = u32(lc, o), u32(lc, o + 4)
        if cmd == 0x19:  # LC_SEGMENT_64
            name = lc[o + 8:o + 24].split(b"\0")[0].decode()
            vm, vs, fo, fs = struct.unpack_from("<QQQQ", lc, o + 24)
            segs[name] = (vm, vs, fo, fs)
            nsect = u32(lc, o + 64)
            for k in range(nsect):
                so = o + 72 + 80 * k
                sn = lc[so:so + 16].split(b"\0")[0].decode()
                sa, ss = struct.unpack_from("<QQ", lc, so + 32)
                segs[name + "," + sn] = (sa, ss, 0, 0)
        elif cmd == 0x2:  # LC_SYMTAB
            symtab = struct.unpack_from("<IIII", lc, o + 8)
        o += sz
    return segs, symtab


def symbols(cache, img_addr, segs, symtab, cache_dir):
    syms = {}
    le = segs["__LINKEDIT"]

    def le_read(off, n):
        return cache.read(le[0] + off - le[2], n)

    if symtab:
        symoff, nsyms, stroff, strsize = symtab
        nl = le_read(symoff, 16 * nsyms)
        st = le_read(stroff, strsize)
        for i in range(nsyms):
            sx, ty, _, _, val = struct.unpack_from("<IBBHQ", nl, 16 * i)
            if ty & 0xe0 or not val:
                continue
            syms.setdefault(val, st[sx:st.index(b"\0", sx)].decode())
    sp = os.path.join(cache_dir, "dyld_shared_cache_arm64.symbols")
    if os.path.exists(sp):
        with open(sp, "rb") as f:
            h = f.read(0x200)
            lso = u64(h, 72)
            f.seek(lso)
            info = f.read(24)
            nlo, nlc, sto, sts, eo, ec = struct.unpack("<6I", info)
            f.seek(lso + eo)
            ents = f.read(16 * ec)
            want = img_addr - cache.base
            for i in range(ec):
                doff, start, cnt = struct.unpack_from("<QII", ents, 16 * i)
                if doff == want:
                    f.seek(lso + nlo + 16 * start)
                    nl = f.read(16 * cnt)
                    for k in range(cnt):
                        sx, ty, _, _, val = struct.unpack_from("<IBBHQ", nl, 16 * k)
                        if ty & 0xe0 or not val:
                            continue
                        f.seek(lso + sto + sx)
                        nm = f.read(256).split(b"\0", 1)[0].decode()
                        syms.setdefault(val, nm)
                    break
    return syms


def emulate(code, pc0, dump=False, stop_ret=True):
    """track x-registers through ADRP/ADR/ADD/SUB/MOV; collect stores [base+off] = value."""
    regs = {}
    stores = []  # (basereg, baseval_or_None, offset, value)
    base_init = {}  # registers holding (argreg, delta) relative to entry args

    for r in range(8):
        base_init[r] = ("x%d" % r, 0)

    def val(r):
        return regs.get(r)

    for i in range(0, len(code) - 3, 4):
        ins = u32(code, i)
        pc = pc0 + i
        rd, rn = ins & 31, (ins >> 5) & 31
        if (ins & 0x9F000000) == 0x90000000:  # ADRP
            imm = sext((((ins >> 5) & 0x7FFFF) << 2) | ((ins >> 29) & 3), 21) << 12
            regs[rd] = (pc & ~0xFFF) + imm
            base_init.pop(rd, None)
        elif (ins & 0x9F000000) == 0x10000000:  # ADR
            imm = sext((((ins >> 5) & 0x7FFFF) << 2) | ((ins >> 29) & 3), 21)
            regs[rd] = pc + imm
            base_init.pop(rd, None)
        elif (ins & 0x7F000000) == 0x11000000 and ins >> 31:  # ADD/SUB imm 64-bit
            imm = ((ins >> 10) & 0xFFF) << (12 if (ins >> 22) & 1 else 0)
            if (ins >> 30) & 1:
                imm = -imm
            if rn in regs:
                regs[rd] = regs[rn] + imm
                base_init.pop(rd, None)
            elif rn in base_init:
                b, d = base_init[rn]
                base_init[rd] = (b, d + imm)
                regs.pop(rd, None)
            else:
                regs.pop(rd, None)
                base_init.pop(rd, None)
        elif (ins & 0xFFE0FFE0) == 0xAA0003E0:  # MOV xd, xm
            rm = (ins >> 16) & 31
            if rm in regs:
                regs[rd] = regs[rm]
            else:
                regs.pop(rd, None)
            if rm in base_init:
                base_init[rd] = base_init[rm]
            else:
                base_init.pop(rd, None)
        elif (ins & 0xFFC00000) == 0xF9000000:  # STR x, [xn, #imm]
            off = ((ins >> 10) & 0xFFF) * 8
            stores.append((base_init.get(rn), off, val(rd), pc))
        elif (ins & 0xFFE00C00) == 0xF8000000:  # STUR x
            off = sext((ins >> 12) & 0x1FF, 9)
            stores.append((base_init.get(rn), off, val(rd), pc))
        elif (ins & 0xFFC00000) in (0xA9000000,):  # STP x, x, [xn, #imm]
            off = sext((ins >> 15) & 0x7F, 7) * 8
            rt2 = (ins >> 10) & 31
            stores.append((base_init.get(rn), off, val(rd), pc))
            stores.append((base_init.get(rn), off + 8, val(rt2), pc))
        elif (ins & 0xFC000000) == 0x94000000:  # BL: clobbers x0-x18
            for r in range(19):
                regs.pop(r, None)
                base_init.pop(r, None)
        elif (ins & 0xFFC00000) == 0xF9400000 and rn in base_init:
            b, d = base_init[rn]  # LDR of a pointer field of an argument: new pseudo-base "[xA+off]"
            base_init[rd] = ("[%s+%#x]" % (b, d + ((ins >> 10) & 0xFFF) * 8), 0)
            regs.pop(rd, None)
        elif (ins & 0xFFC00000) in (0xF9400000,) or (ins & 0xFF000000) == 0x58000000:
            regs.pop(rd, None)  # LDR: unknown value
            base_init.pop(rd, None)
        elif ins == 0xD65F03C0 and stop_ret:  # RET
            break
        elif (ins & 0x1F000000) in (0x0B000000, 0x0A000000, 0x12000000, 0x13000000) or \
                (ins & 0x1F800000) == 0x12800000:
            regs.pop(rd, None)  # other data-processing writing rd
            base_init.pop(rd, None)
        if dump and stores and stores[-1][3] == pc:
            print("  %#x store %s" % (pc, stores[-1]))
    return stores


def thunk_slot(code):
    """OpenGLES exported gl* thunk: mrs xC, TPIDRRO_EL0; ldr xC,[xC,#240]; ...; ldr xF,[xC,#off]; ldr x0,[xC,#32];
    br xF.  The context block is {.., +32 rctx, +40 disp[0], ...}; slot = (off - 40) / 8."""
    ctx, tls, fnoff, got_rctx = set(), set(), None, False
    for i in range(0, min(len(code), 4 * 40), 4):
        ins = u32(code, i)
        rd, rn = ins & 31, (ins >> 5) & 31
        if (ins & 0xFFFFFFE0) == 0xD53BD060:  # mrs xd, TPIDRRO_EL0
            tls.add(rd)
        elif (ins & 0xFFC00000) == 0xF9400000:  # ldr x, [xn, #imm]
            off = ((ins >> 10) & 0xFFF) * 8
            if rn in tls and off == 240:
                ctx.add(rd)
                tls.discard(rd)
            elif rn in ctx and off == 32 and rd == 0:
                got_rctx = True
            elif rn in ctx and off >= 40 and fnoff is None:
                fnoff = (off, rd)
            else:
                ctx.discard(rd)
        elif (ins & 0xFFE0FFE0) == 0xAA0003E0:  # mov xd, xm
            rm = (ins >> 16) & 31
            if rm in ctx:
                ctx.add(rd)
            else:
                ctx.discard(rd)
        elif (ins & 0xFFFFFC1F) in (0xD61F0000, 0xD63F0000):  # br/blr xn
            if fnoff and got_rctx and rn == fnoff[1] and (fnoff[0] - 40) % 8 == 0:
                return (fnoff[0] - 40) // 8
            return None
        elif ins == 0xD65F03C0:
            return None
    return None


def image_syms(c, cdir, suffix):
    hits = [(a, p) for a, p in c.images() if p.endswith(suffix)]
    if not hits:
        raise SystemExit("image %s not in cache" % suffix)
    a, p = hits[0]
    segs, st = macho(c, a)
    return a, p, segs, symbols(c, a, segs, st, cdir)


SUFFIX_RANK = ["", "ARB", "EXT", "OES", "APPLE", "KHR", "NV", "IMG"]


def rank(name):
    for i, s in enumerate(SUFFIX_RANK[1:], 1):
        if name.endswith(s):
            return (i, name)
    return (0, name)


def init_table_stores(c, syms, text):
    """cross-check: the GLEngine function with the most 'str <text pointer>, [xT, #off]' stores is _gliInitDispatchTable
    (its name is <redacted> in production caches). Its stores are 'ldr x9,[x10,#off]; cmp; csel x9,x11,x10,eq;
    adrp/add x12, fn; str x12,[x9,#off]' (two tables, same offset), so the base register is not tracked: only the
    offset is kept. Returns (func addr, {byte offset: fn})."""
    addrs = sorted(a for a in syms if text[0] <= a < text[0] + text[1])
    best = (0, [], {})
    for i, a in enumerate(addrs):
        end = addrs[i + 1] if i + 1 < len(addrs) else text[0] + text[1]
        if end - a > 0x40000:
            continue
        tab = {}
        for base, off, v, pc in emulate(c.read(a, end - a), a, stop_ret=False):
            if v is not None and off % 8 == 0 and text[0] <= v < text[0] + text[1] and not (base and base[1]):
                tab[off] = v
        if len(tab) >= 50:  # the init is split into several functions (one per API level / extension group)
            best[1].append((a, len(tab)))
            best[2].update(tab)
    return best[1], best[2]


CTX_RCTX, CTX_DISP = 32, 40  # OpenGLES TLS context: +32 rctx, +40 disp[0] (read from the thunks)


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    cdir = args[0]
    c = Cache(cdir)
    _, gp, gsegs, gsyms = image_syms(c, cdir, "OpenGLES.framework/OpenGLES")
    slots = {}
    for addr, name in gsyms.items():
        if not re.match(r"^_gl[A-Z]", name):
            continue
        s = thunk_slot(c.read(addr, 160))
        if s is not None:
            slots.setdefault(s, set()).add(name[1:])
    log = sys.stderr.write
    log("# %s: %d gl* thunks -> %d slots (max %d)\n" % (gp, sum(len(v) for v in slots.values()), len(slots), max(slots)))
    if "--no-check" not in args:
        _, ep, esegs, esyms = image_syms(c, cdir, "GLEngine.bundle/GLEngine")
        fa, tab = init_table_stores(c, esyms, esegs["__TEXT,__text"][:2])
        init = {(o - CTX_DISP) // 8 for o in tab if o >= CTX_DISP}
        log("# %s: dispatch init functions %s store %d pointers -> slots %d..%d\n" % (ep, [(hex(x), n) for x, n in fa], len(tab), min(init or [0]), max(init or [0])))
        miss = sorted(s for s in slots if s not in init)
        log("#   thunk slots NOT stored by init: %d %s\n" % (len(miss), miss[:20]))
        log("#   init slots without an exported gl* thunk (unnamed): %d\n" % len(init - set(slots)))
    print("# GLI slot map (iOS OpenGLES/GLEngine dyld cache), generated by gli_slots.py: '<slot> <glName>'")
    for s in sorted(slots):
        names = sorted(slots[s], key=rank)
        print("%d %s%s" % (s, names[0], ("  # alias " + " ".join(names[1:])) if len(names) > 1 else ""))


if __name__ == "__main__":
    main()
