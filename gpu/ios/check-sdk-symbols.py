#!/usr/bin/env python3
# PixOS G3: check that a Mach-O links only libraries of an iOS SDK and that every undefined symbol is exported by one of them.
# Usage: check-sdk-symbols.py SDK BINARY...   (needs llvm-objdump / llvm-nm on PATH, e.g. inside gpu/nix/shell.nix)
# For each binary: the LC_LOAD_DYLIB list (each must map to a .tbd under SDK), then every undefined symbol (nm -u) is looked up in
# the symbols those .tbd files export (symbols, weak-symbols, thread-local-symbols, objc-classes/-eh-types/-ivars, following
# re-exported libraries). Flat-namespace or dynamic_lookup binding is not accepted. Exit 0 = all resolved.
import os
import re
import subprocess
import sys


def tbd_path(sdk, install_name):
    base = os.path.splitext(install_name)[0] if install_name.endswith('.dylib') else install_name
    for cand in (sdk + base + '.tbd', sdk + install_name + '.tbd'):
        if os.path.exists(cand):
            return cand
    return None


def parse_tbd(path, target="arm64-ios"):
    """(install names, exported symbols, re-exported install names) of a tbd v4 file, for one target only: each document
    (--- !tapi-tbd) has top-level sections (exports, reexports, reexported-libraries) made of "- targets: [...]" items"""
    syms, reexports, names = set(), set(), set()
    for doc in open(path).read().split("--- !tapi-tbd")[1:]:
        m = re.search(r"^install-name:\s*\x27?([^\x27\n]+)\x27?", doc, re.M)
        if m:
            names.add(m.group(1).strip())
        section, items = None, []
        for line in doc.splitlines():
            if re.match(r"^[a-z-]+:", line):
                section = line.split(":")[0]
                continue
            if re.match(r"^\s+- targets:", line):
                items.append([section, line])
            elif items and line.startswith(" "):
                items[-1][1] += "\n" + line
        for section, body in items:
            tm = re.search(r"targets:\s*\[([^\]]*)\]", body)
            if not tm or target not in [t.strip() for t in tm.group(1).split(",")]:
                continue
            for lm in re.finditer(r"([a-z-]+):\s*\[([^\]]*)\]", body, re.S):
                key = lm.group(1)
                vals = [i.strip().strip("\x27\"") for i in lm.group(2).replace("\n", " ").split(",") if i.strip()]
                if section == "reexported-libraries" and key == "libraries":
                    reexports.update(vals)
                elif section not in ("exports", "reexports"):
                    continue
                elif key in ("symbols", "weak-symbols", "thread-local-symbols"):
                    syms.update(vals)
                elif key == "objc-classes":
                    for c in vals:
                        syms.update({"_OBJC_CLASS_$_" + c.lstrip("_"), "_OBJC_METACLASS_$_" + c.lstrip("_")})
                elif key == "objc-eh-types":
                    syms.update("_OBJC_EHTYPE_$_" + c.lstrip("_") for c in vals)
                elif key == "objc-ivars":
                    syms.update("_OBJC_IVAR_$_" + c.lstrip("_") for c in vals)
    return names, syms, reexports


def exported(sdk, install_name, seen):
    if install_name in seen:
        return set()
    seen.add(install_name)
    p = tbd_path(sdk, install_name)
    if not p:
        return None
    names, syms, reexports = parse_tbd(p)
    seen.update(names)
    for r in reexports:
        if r in names:
            continue
        sub = exported(sdk, r, seen)
        if sub:
            syms |= sub
    return syms


def main():
    sdk = sys.argv[1].rstrip('/')
    # negative control: a Linux-only symbol must not resolve against libSystem (catches a parser that accepts everything)
    if '_eventfd' in (exported(sdk, '/usr/lib/libSystem.B.dylib', set()) or set()):
        print('FAIL: negative control _eventfd found in libSystem')
        return 1
    rc = 0
    for binary in sys.argv[2:]:
        out = subprocess.run(['llvm-objdump', '--macho', '--dylibs-used', binary], capture_output=True, text=True, check=True).stdout
        libs = [l.strip().split(' (')[0] for l in out.splitlines()[1:] if l.strip()]
        # a dylib lists its own LC_ID_DYLIB first: that is the image itself, not a dependency
        ident = subprocess.run(['llvm-objdump', '--macho', '--dylib-id', binary], capture_output=True, text=True).stdout
        ident = [l.strip() for l in ident.splitlines()[1:] if l.strip()]
        if ident:
            print('  id %s (self, not checked)' % ident[0])
            libs = [l for l in libs if l != ident[0]]
        hdr = subprocess.run(['llvm-objdump', '--macho', '--private-header', binary], capture_output=True, text=True,
                             check=True).stdout
        print('%s:' % binary)
        if 'TWOLEVEL' not in hdr:
            print('  FAIL: not a two-level-namespace image')
            rc = 1
        avail = set()
        for lib in libs:
            syms = exported(sdk, lib, set())
            if syms is None:
                print('  FAIL: %s has no .tbd in the SDK' % lib)
                rc = 1
            else:
                print('  dylib %s: %d symbols in SDK .tbd' % (lib, len(syms)))
                avail |= syms
        und = subprocess.run(['llvm-nm', '-u', '-j', binary], capture_output=True, text=True, check=True).stdout.split()
        missing = sorted(s for s in set(und) if s not in avail and s != 'dyld_stub_binder')
        print('  undefined symbols: %d, unresolved: %d' % (len(set(und)), len(missing)))
        for s in missing:
            print('    UNRESOLVED %s' % s)
        if missing:
            rc = 1
    print('RESULT %s' % ('PASS' if rc == 0 else 'FAIL'))
    return rc


if __name__ == '__main__':
    sys.exit(main())
