"""Compile the kernel's pure archive/path/inflate functions (xnu patch 0117 + 0163 on top, between the host-test markers) on the host with
ASan/UBSan and compare them with the Python twin tools/mkdevoverlay.py on a seeded corpus; then check that known
mutants of the C code are caught (review 18 finding 4). The inflate driver sd_stream (0163) is linked against host zlib.
Skips when gcc/cc is missing."""
import glob, os, random, re, shutil, struct, subprocess, sys, tempfile, unittest, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkdevoverlay as m

PATCH = os.path.join(HERE, '..', 'xnu', 'patches', '0117-sweet-dev-overlay.patch')
PATCH_Z = os.path.join(HERE, '..', 'xnu', 'patches', '0163-sweet-dev-overlay-z.patch')
CC = shutil.which('gcc') or shutil.which('cc')

MAIN = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <zlib.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define SD_ZALLOC Z_NULL
#define SD_ZFREE Z_NULL
%s
static uint64_t g_usize;
/* discard sink; aborts if sd_stream ever hands it a byte past usize (bomb cut-off) */
static int sink(void *ctx, const uint8_t *p, uint32_t n, uint64_t off) {
	(void)ctx; (void)p;
	if (n == 0 || n > 4096 || off + n > g_usize) abort();
	return 0;
}
/* stdin: repeated [u32 len][len bytes]; one result block per archive, ending "--" */
int main(void) {
	uint32_t n;
	(void)sd_crc32;
	while (fread(&n, 4, 1, stdin) == 1) {
		uint8_t *b = malloc(n ? n : 1);                    /* exact size: ASan sees any read past the end */
		if (fread(b, 1, n, stdin) != n) return 2;
		uint32_t count = 0, ver = 0;
		if (sd_hdr(b, n, &count, &ver) || sd_walk(b, n, ver, count)) { puts("ERR"); }
		else {
			uint64_t off = SD_HDR; struct sd_ent e;
			for (uint32_t i = 0; i < count; i++) {
				sd_next(b, n, ver, &off, &e);
				static uint8_t zb[4096]; const char *zw; g_usize = e.usize;
				int sok = sd_stream(&e, zb, sizeof(zb), sink, NULL, &zw) == 0;
				char path[SD_MAXPATH + 1], tmp[SD_MAXPATH + 16], orig[SD_MAXPATH + 16], sh[160]; size_t mk = 0;
				const char *why = sd_path_check(&e, path, &mk);
				tmp[0] = orig[0] = 0;
				if (!why && sd_names(path, tmp, sizeof(tmp), orig, sizeof(orig))) why = "names";
				sd_show(&e, sh, sizeof(sh));                /* exact-size array: ASan sees a one-byte overflow */
				char t2[40], o2[40];                        /* too-small buffers: sd_names must refuse, never truncate */
				int small = why ? 0 : sd_names(path, t2, sizeof(t2), o2, sizeof(o2)) != 0;
				printf("E %%s|%%u|%%u|%%u|%%llu|%%d|%%lld|%%s|%%s|%%d|%%s\n", why ? why : "ok", e.mode, e.uid, e.gid, (unsigned long long)e.usize,
				    sok, why ? 0LL : (long long)mk, tmp, orig, small, sh);
			}
		}
		puts("--"); free(b);
	}
	return 0;
}
'''


def kernel_file():
    """bsd/dev/sweet_devoverlay.c as 0117 creates it with 0163 applied on top (git apply outside a repo)."""
    s = open(PATCH).read()
    s = s[s.index('+++ b/bsd/dev/sweet_devoverlay.c'):]
    lines = [l[1:] for l in s.split('\n')[2:] if l.startswith('+')]
    d = tempfile.mkdtemp(); os.makedirs(os.path.join(d, 'bsd', 'dev'))
    try:
        f = os.path.join(d, 'bsd', 'dev', 'sweet_devoverlay.c')
        open(f, 'w').write('\n'.join(lines) + '\n')
        r = subprocess.run(['git', 'apply', '--unsafe-paths', os.path.abspath(PATCH_Z)], cwd=d, capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        return open(f).read()
    finally:
        shutil.rmtree(d, True)


def kernel_source():
    src = kernel_file()
    a, b = src.index('/* host-test begin'), src.index('/* host-test end */')
    return src[a:b]


_ZF = []


def zlib_flags():
    """Compiler flags for host zlib: $ZLIB_CFLAGS/$ZLIB_LIBS, else the first candidate that links (default paths, then a
    Nix store zlib: -dev headers + a lib of the host architecture)."""
    if _ZF:
        return _ZF[0]
    if os.environ.get('ZLIB_CFLAGS') or os.environ.get('ZLIB_LIBS'):
        _ZF.append(os.environ.get('ZLIB_CFLAGS', '').split() + os.environ.get('ZLIB_LIBS', '-lz').split())
        return _ZF[0]
    cands = [['-lz']]
    for inc in sorted(glob.glob('/nix/store/*-zlib-*-dev/include'), reverse=True):
        ver = os.path.basename(os.path.dirname(inc))[33:-4]                  # <hash>-zlib-1.3.2-dev -> zlib-1.3.2
        for lib in sorted(glob.glob('/nix/store/*-%s/lib/libz.so' % ver)):
            d = os.path.dirname(lib)
            cands.append(['-I' + inc, '-L' + d, '-Wl,-rpath,' + d, '-lz'])
    t = tempfile.mkdtemp()
    try:
        c = os.path.join(t, 'z.c')
        open(c, 'w').write('#include <zlib.h>\nint main(void){return zlibVersion()[0]==0;}\n')
        for f in cands:
            if subprocess.run([CC, '-o', os.path.join(t, 'z'), c, *f], capture_output=True).returncode == 0:
                _ZF.append(f)
                return f
    finally:
        shutil.rmtree(t, True)
    _ZF.append(['-lz'])
    return _ZF[0]


def build(src, out):
    code = MAIN % src
    c = out + '.c'
    open(c, 'w').write(code)
    flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']   # no fallback without sanitizers (review 19 M1-new d)
    r = subprocess.run([CC, '-O1', '-Wall', '-Werror=implicit-function-declaration', *flags, '-o', out, c, *zlib_flags()], capture_output=True, text=True)
    return None if r.returncode == 0 else (r.stderr[-2000:] or 'sanitizer build failed')


def py(blob):
    try:
        ents = m.parse(blob, with_usize=True)
    except ValueError:
        return ['ERR', '--']
    return ['E %s|%d|%d|%d|%d|%d|%d|%s|%s|%d|%s' % (m.path_check(p) or 'ok', mo, u, g, us, int(c), mkmin(p) if not m.path_check(p) else 0,
            *(((p + '.sweettmp', p + '.sweetorig') if not m.path_check(p) else ('', ''))), int(len(p) + 10 >= 40 and not m.path_check(p)), shown(p)) for p, mo, u, g, d, c, us in ents] + ['--']


def mkmin(p):
    """Twin of *mkmin: len(prefix)-1 under a K_DIR prefix, -1 (size_t max) for K_NAME/K_EXACT."""
    for pfx in ('/usr/lib/', '/usr/local/', '/private/var/dev/'):
        if p.startswith(pfx):
            return len(pfx) - 1
    return -1


def shown(p):
    b = p.encode('utf-8', 'surrogateescape')[:159]            # sd_show with a 160-byte buffer
    return ''.join(chr(c) if 0x20 <= c < 0x7f else '?' for c in b)


def raw(plen, mode, uid, gid, crc, size, path, data=b''):
    e = struct.pack('<HHIIIQ', plen, mode, uid, gid, crc, size) + path
    e += b'\0' * (-len(e) % 16)
    e = e + data
    return e + b'\0' * (-len(e) % 16)


def hdr(count, ver=1):
    return b'SWEETDEV' + struct.pack('<II', ver, count)


def raw2(path, body, usize, crc, flags=1, zero=0, plen=None, size=None):
    e = struct.pack('<HHIIIQQII', len(path) if plen is None else plen, 0o644, 0, 0, crc, len(body) if size is None else size, usize, flags, zero) + path
    e += b'\0' * (-len(e) % 16)
    e = e + body
    return e + b'\0' * (-len(e) % 16)


def zcases():
    """version 2 / zlib cases (patch 0163): good, truncated, wrong crc, bomb, caps, bad fields, trailing data."""
    p, data = b'/usr/lib/z', bytes(range(256)) * 64 + b'tail'          # 16388 bytes: several 4096-byte sink pieces
    z, crc = zlib.compress(data, 9), zlib.crc32(data)
    big = b'\0' * (3 << 20); zb = zlib.compress(big, 9)
    out = [hdr(1, 2) + raw2(p, z, len(data), crc),                       # good
           hdr(2, 2) + raw2(p, z, len(data), crc) + raw2(b'/usr/lib/s', b'abc', 3, zlib.crc32(b'abc'), flags=0),   # good + stored
           hdr(1, 2) + raw2(p, z[:-7], len(data), crc),                  # truncated stream
           hdr(1, 2) + raw2(p, z[:len(z) // 2], len(data), crc),
           hdr(1, 2) + raw2(p, z, len(data), crc ^ 1),                   # wrong crc
           hdr(1, 2) + raw2(p, z, len(data) + 1, crc),                   # declared one byte too long
           hdr(1, 2) + raw2(p, z, len(data) - 1, crc),                   # declared one byte short
           hdr(1, 2) + raw2(p, zb, 4096, zlib.crc32(big[:4096])),        # bomb: declared 4 KiB, inflates to 3 MiB
           hdr(1, 2) + raw2(p, zb, 0, 0),                                # bomb: declared 0
           hdr(1, 2) + raw2(p, z + b'X', len(data), crc),                # data after the stream
           hdr(1, 2) + raw2(p, z + zlib.compress(b'x'), len(data), crc),
           hdr(1, 2) + raw2(p, b'not zlib at all', len(data), crc),
           hdr(1, 2) + raw2(p, b'', 0, 0),                               # empty zlib body
           hdr(1, 2) + raw2(p, zlib.compress(b''), 0, 0),                # empty file, zlib
           hdr(1, 2) + raw2(p, z, (40 << 20) + 1, crc),                  # usize over the cap: archive refused
           hdr(1, 2) + raw2(p, z, 40 << 20, crc),                        # at the cap: structure ok, length mismatch
           hdr(2, 2) + raw2(p, z, 30 << 20, crc) + raw2(b'/usr/lib/y', z, 30 << 20, crc),   # total over the cap
           hdr(1, 2) + raw2(p, z, 2 ** 64 - 1, crc),
           hdr(2, 2) + raw2(p, z, 2 ** 64 - 1, crc) + raw2(b"/usr/lib/y", z, 2, crc),   # per-entry cap keeps the total from wrapping
           hdr(1, 2) + raw2(p, z, len(data), crc, flags=2),
           hdr(1, 2) + raw2(p, z, len(data), crc, zero=1),
           hdr(1, 2) + raw2(p, b'abc', 4, zlib.crc32(b'abc'), flags=0),  # stored with usize != size
           hdr(1, 2) + raw2(p, z, len(data), crc, size=2 ** 64 - 1),
           hdr(1, 2) + raw2(b'/etc/z', z, len(data), crc),               # bad path, stream still checked
           hdr(1, 3) + raw2(p, z, len(data), crc),
           hdr(1, 1) + raw2(p, z, len(data), crc)]                       # v2 header bytes under version 1
    good = out[0]
    for cut in (17, 40, 55, 56, len(good) - 1, len(good) - 16):
        out.append(good[:cut])
    R = random.Random(163)
    for _ in range(400):
        ents = [(R.choice(['/usr/lib/a', '/usr/local/b/c', '/etc/x']), 0o644, 0, 0,
                 bytes(R.randrange(4) for _ in range(R.randrange(0, 9000)))) for _ in range(R.randrange(1, 3))]
        blob = bytearray(m.pack(ents, zlib_above=R.choice([0, 100, 5000])))
        for _ in range(R.randrange(0, 3)):
            blob[R.randrange(len(blob))] = R.randrange(256)
        if R.random() < 0.3:
            blob = blob[:R.randrange(len(blob) + 1)]
        out.append(bytes(blob))
    return out


def corpus():
    R = random.Random(18)
    paths = ['/usr/lib/a', '/usr/lib/../x', '/usr/lib//x', '/usr/libx', '/usr/local/a/b', '/private/var/dev/h', '/etc/x',
             '/System/Library/xpc/launchd.plist', '/System/Library/LaunchDaemons/org.pixelios.', '/System/Library/LaunchDaemons/org.pixelios.t', '/System/Library/LaunchDaemons/org.pixelios.d/x',
             '/usr/lib/a.sweetorig', '/usr/lib/a.SweetOrig', '/usr/lib/a.SWEETTMP', '/usr/lib/a\x00b', '/usr/lib/a\x7fb', '/usr/lib/a\x1fb', '/',
             '/usr/lib/', '/usr/lib/.', '/usr/lib/a/', '/usr/lib/\xe9', '/usr/lib/' + 'n' * 245, '/usr/lib/' + 'n' * 246, '/usr/lib/x/..', '/usr/lib/x/../y',
             '/System/Library/xpc/launchd.plistX', '/System/Library/xpc/launchd.plist/x', '/usr/lib/' + 'a' * 291,
             '/usr/lib/' + 'b' * 20, '/usr/lib/' + 'b' * 21]   # last: 300 bytes, sd_show cap 160
    out = []
    # crafted: overflow and bound cases
    p = b'/usr/lib/a'
    for size in (2**64 - 1, 2**64 - 8, 2**64 - 16, 2**63, 2**32, 4096):
        out.append(hdr(1) + raw(len(p), 0o644, 0, 0, 0, size, p, b'abc'))
    for plen in (0, 1001, 65535, 11, 2000):
        out.append(hdr(1) + raw(plen, 0o644, 0, 0, 0, 3, p, b'abc'))
    out.append(hdr(1) + raw(1001, 0o644, 0, 0, 0, 0, b'/usr/lib/' + b'a' * 992) + b'\0' * 64)   # 1001-byte path with room
    out.append(hdr(4097) + b''.join(raw(len(p), 0o644, 0, 0, 0, 0, p) for _ in range(4097)))
    out.append(hdr(4096) + b''.join(raw(len(p), 0o644, 0, 0, 0, 0, p) for _ in range(4096)))
    out.append(hdr(0xffffffff)); out.append(hdr(1, 2)); out.append(hdr(1, 3)); out.append(b'SWEETDEV'); out.append(b'')
    good = m.pack([('/usr/lib/a', 0o644, 0, 0, b'abc')])
    out.append(good[:-1]); out.append(good[:-16]); out.append(good[:40])
    for q in paths:                                           # every path alone, with and without the final padding
        blob = m.pack([(q, 0o644, 0, 0, b'zz')])
        out.append(blob); out.append(blob[:-1])
    for _ in range(1500):
        ents = [(R.choice(paths), R.randrange(0, 0o1000), R.randrange(0, 600), 0, bytes(R.randrange(256) for _ in range(R.randrange(40))))
                for _ in range(R.randrange(1, 4))]
        blob = bytearray(m.pack(ents))
        for _ in range(R.randrange(0, 4)):
            blob[R.randrange(len(blob))] = R.randrange(256)
        if R.random() < 0.3:
            blob = blob[:R.randrange(len(blob) + 1)]
        if R.random() < 0.2:
            blob += bytes(R.randrange(40))
        out.append(bytes(blob))
    return out + zcases()


def run_c(exe, blobs):
    data = b''.join(struct.pack('<I', len(b)) + b for b in blobs)
    try:
        r = subprocess.run([exe], input=data, capture_output=True, timeout=60)
    except subprocess.TimeoutExpired:                         # a mutant that loops forever (e.g. no-progress inflate) counts as caught
        return -1, [], "timeout"
    return r.returncode, r.stdout.decode('utf-8', 'surrogateescape').split('\n')[:-1], r.stderr.decode(errors='replace')[:400]


def mismatches(exe, blobs):
    """Number of archives where C and Python disagree (a crash/ASan abort counts as total mismatch)."""
    rc, out, err = run_c(exe, blobs)
    if rc != 0:
        return len(blobs)
    got, cur = [], []
    for l in out:
        cur.append(l)
        if l == '--':
            got.append(cur); cur = []
    if len(got) != len(blobs):
        return len(blobs)
    return sum(1 for b, g in zip(blobs, got) if py(b) != g)


MUTANTS = [
    # no 'plen > len - off' mutant: equivalent, the aligned-offset check right after it rejects the same archives (plen <= 1000, no wrap)
    ('plen max', 'e->plen > SD_MAXPATH', 'e->plen > 70000'),
    ('size bound', 'if (off > len || e->size > len - off)', 'if (off > len)'),
    ('final padding check', 'if (off > len) {\n\t\treturn EINVAL;\n\t}\n\t*offp', '*offp'),
    ('count max', 'rd32(b + 12) > SD_MAXCOUNT', 'rd32(b + 12) > 0x7fffffffu'),
    ('header len', 'len < SD_HDR || ', ''),
    ('version', '(rd32(b + 8) != SD_VERSION && rd32(b + 8) != SD_VERSION2)', '0'),
    # patch 0163: inflate driver and version 2 fields
    ('v2 reserved', 'rd32(p + 36) != 0 || ', ''),
    ('v2 flags', 'e->flags > SD_F_ZLIB', '0'),
    ('v2 stored usize', '(e->flags == 0 && e->usize != e->size) || ', ''),
    ('v2 usize cap', 'e->usize > SD_MAXUSIZE)', '0)'),
    ('v2 total cap', 'total > SD_MAXUSIZE', 'total > SD_MAXUSIZE * 2'),
    ('bomb cut-off', 'got > e->usize - out', '0'),
    ('length check', 'if (out != e->usize) {', 'if (0) {'),
    ('crc check', 'if (~c != e->crc) {', 'if (0) {'),
    ('trailing data', 'in != e->size || z.avail_in != 0', '0'),
    ('zlib error', 'if (zr != Z_OK && zr != Z_STREAM_END) {', 'if (zr < Z_BUF_ERROR) {'),
    ('crc of inflated', 'c = sd_crc_upd(c, buf, got);', ''),
    ('control byte', 'c < 0x20 ||', 'c < 0x1f ||'),
    ('NAME slash', "memchr(out + pl, '/', n - pl)", '0'),
    ('dotdot', "len == 2 && out[comp + 1] == '.' && out[comp + 2] == '.'", '0'),
    ('dot', "len == 1 && out[comp + 1] == '.'", '0'),
    ('empty component', 'if (len == 0) {', 'if (0) {'),
    ('suffix case', "if (c >= 'A' && c <= 'Z') {", 'if (0) {'),
    ('component 245', 'len > 245', 'len > 255'),
    ('crc poly', '0xedb88320u', '0xedb88321u'),
    # review 19 M1-new: mutants that survived the first version
    ('mkmin D', '*mkmin = pl - 1;', '*mkmin = 0;'),
    ('mkmin NAME', "memchr(out + pl, '/', n - pl)) {\n\t\t\t\tcontinue;\n\t\t\t}\n\t\t\t*mkmin = ((size_t)-1);", "memchr(out + pl, '/', n - pl)) {\n\t\t\t\tcontinue;\n\t\t\t}\n\t\t\t*mkmin = 0;"),
    ('NAME exact prefix', 'if (n == pl || memchr', 'if (memchr'),   # review 20 f3
    ('EXACT length', 'if (n != pl) {', 'if (0) {'),
    ('sd_show cap', 'e->plen < cap - 1 ? e->plen : cap - 1', 'e->plen < cap ? e->plen : cap'),
    ('C1 sizeof(tmp)', 'snprintf(tmp, tcap,', 'snprintf(tmp, sizeof(tmp),'),
    ('C1 sizeof(orig)', 'snprintf(orig, ocap,', 'snprintf(orig, sizeof(orig),'),
    ('C1 no truncation check', '(a < 0 || (size_t)a >= tcap || b < 0 || (size_t)b >= ocap)', '0'),
    # no tmp-only truncation mutant: equivalent, the longer .sweetorig always truncates first
    ('non-ASCII byte', 'c >= 0x7f', 'c == 0x7f'),
    ('absolute', "e->path[0] != '/'", '0'),
]


@unittest.skipUnless(CC, 'no gcc/cc on this host: kernel-function differential test skipped')
class KernelTwin(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(); cls.addClassCleanup(shutil.rmtree, cls.tmp, True)
        cls.src = kernel_source()
        cls.blobs = corpus()

    @staticmethod
    def _sizeof_hits(t):
        src = t[t.index('+++ b/bsd/dev/sweet_devoverlay.c'):]
        assert src.count('/* host-test end */') == 1
        return re.search(r'sizeof\((shown|path|tmp|orig)\)', src.split('/* host-test end */')[1])

    def test_no_sizeof_of_pointer_buffers(self):
        # review 19 C1: sd_one's buffers are char * into sd_w; sizeof(tmp) etc. is 8. Review 20 f1: scan the region AFTER the end marker.
        t = open(PATCH).read()
        self.assertIsNone(self._sizeof_hits(t))
        self.assertIn('sd_names(path, tmp, sizeof(sd_w.tmp), orig, sizeof(sd_w.orig))', t)
        a = 'sd_show(e, shown, '
        self.assertEqual(t.count(a), 1)
        mut = t.replace(a, 'sd_show(e, shown, sizeof(shown)), (void)(')   # mutant: sizeof(shown) inside sd_one
        self.assertIsNotNone(self._sizeof_hits(mut), 'sizeof(shown) mutant not caught')

    def test_markers_present(self):
        self.assertIn('sd_next', self.src); self.assertIn('sd_path_check', self.src)

    def test_matches_python_twin(self):
        exe = os.path.join(self.tmp, 'h')
        err = build(self.src, exe)
        self.assertIsNone(err, err)
        self.assertEqual(mismatches(exe, self.blobs), 0)

    def test_mutants_are_caught(self):
        alive = []
        for i, (name, a, b) in enumerate(MUTANTS):
            self.assertEqual(self.src.count(a), 1, 'mutant %r no longer matches the kernel source' % name)
            exe = os.path.join(self.tmp, 'm%d' % i)
            err = build(self.src.replace(a, b), exe)
            self.assertIsNone(err, '%s: %s' % (name, err))
            if mismatches(exe, self.blobs) == 0:
                alive.append(name)
        self.assertEqual(alive, [], 'mutants not caught: %s' % alive)


if __name__ == '__main__':
    unittest.main()
