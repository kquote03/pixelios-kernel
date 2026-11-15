"""Source audit of xnu patch 0148 (sweet-hsi2c-fg, the READ-only fuel-gauge reader, boot-arg sweetbatt=1). Additive: it loosens nothing else.

Pins, on the text the kernel gets (the new-file hunks of the patch):
  bsd/dev/sweet_hsi2c_fg.h  ONE writer: ops->wr32 is called exactly once, inside sfg_w32(), after sfg_reg_writable() (first statement) and
                            sfg_w32_allowed(); the decision functions are SFG_NOINL and their text is pinned (sha256 of the whitespace-free
                            body, so any change shows up here); exactly 16 sfg_w32() call sites (7 in sfg_msg_start, 3 in sfg_xfer, 6 in
                            sfg_restore); the device allowlist {0x36} and the seven register pointers with their static asserts; no MMIO
                            (no volatile, no hex literal >= 0x10000), no goto, loops only in the bounded waits and the text helpers;
                            the order of sfg_set (stop latch, counter, gate, pads, controller words, preconditions, reads, restore).
  bsd/dev/sweet_hsi2c.c     the boot-arg check comes before any ADT lookup or mapping; exactly one MMIO store (sb.i2c[...] = v in sb_wr32);
                            no hex literal >= 0x10000 (every address comes from the ADT); sb_wr32 is only the ops.wr32 hook.
  review F5 (2026-11-15): the failure-time diagnostics (sfg_stage, sfg_late, sfg_diag_lines and the name tables) contain no bus access, and
                            sfg_poll still reads exactly its four status words.
Every check has a killing mutant below (CR-01/CR-02): the mutants run on a private copy of the text, never on the file."""
import hashlib
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH = os.path.join(ROOT, "xnu", "patches", "0148-sweet-hsi2c-fg.patch")

# sha256[:16] of the whitespace-free text of each pinned function (signature through closing brace, comments stripped)
PINNED = {
    "sfg_w32_allowed": "343886a3aece0816",
    "sfg_w32": "d67bb34482274caa",
    "sfg_reg_writable": "1d158f119ae3aa13",
    "sfg_rd_ok": "b6b7959a760d7362",
    "sfg_derive": "505319c0f7f55498",
    "sfg_restore": "182e9de8d5de0440",
    "sfg_msg_start": "24cd1f91ce953cc1",
}
F5_NOBUS = ("sfg_stage", "sfg_stage_name", "sfg_why_name", "sfg_diag_lines", "sfg_late")
W32_SITES = {"sfg_msg_start": 7, "sfg_xfer": 3, "sfg_restore": 6}
REGS_TEXT = ("#define SFG_REGS(X) X(REPSOC, 0x06u) X(TEMP, 0x08u) X(VCELL, 0x09u) X(CURRENT, 0x0Au) X(AVGCURRENT, 0x0Bu) X(FULLCAPREP, 0x35u) "
             "X(DEVNAME, 0x21u)")
# every loop of the header, per function: fixed-count loops (cfg checks, the seven registers, two bytes), the text helpers, and the three
# waits that are bounded by SFG_POLLS / SFG_TICK_SPINS AND the counter
LOOPS_OK = {"sfg_cfg_load": 4, "sfg_txt": 1, "sfg_hex": 1, "sfg_dec": 2, "sfg_wait": 1, "sfg_xfer": 1, "sfg_idle_wait": 1, "sfg_counter_runs": 1,
            "sfg_set": 3}
SET_ORDER = ["if(st->stopped)return1;", "if(!o->freq_hz)returnsfg_refuse(", "if(!sfg_counter_runs(&r))returnsfg_refuse(", "why=sfg_gate(&r);",
             "why=sfg_pads(&r);", "if(!sfg_r32(&r,SFG_SP_I2C,c->o_ctl,&r.s.ctl)", "why=sfg_precondition(c,&r.s);", "if(why)returnsfg_refuse(st,o,why);",
             "for(i=0;i<SFG_NREGS&&e==SFG_OK;i++){", "restored=sfg_restore(&r,first||e!=SFG_OK);"]


def new_file(text, path):
    marker = "+++ b/" + path
    if text.count("\n" + marker + "\n") != 1:
        raise AssertionError(f"0148: want exactly one new-file hunk for {path}")
    out, on = [], False
    for line in text.splitlines():
        if line == marker:
            on = True
            continue
        if on and line.startswith("diff --git "):
            break
        if on and line.startswith("@@"):
            continue
        if on and line.startswith("+"):
            out.append(line[1:])
    return "\n".join(out) + "\n"


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def func_span(code, name):
    """(start of the signature line, end after the closing brace) of the one definition of NAME (column-0 name followed by '(')."""
    hits = [m.start() for m in re.finditer(r"(?m)^" + re.escape(name) + r"\(", code)]
    if len(hits) != 1:
        raise AssertionError(f"want exactly one definition of {name}, found {len(hits)}")
    start = code.rfind("\n", 0, max(0, code.rfind("\n", 0, hits[0]))) + 1     # include the return-type line
    i = code.index("{", hits[0])
    depth = 0
    for j in range(i, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return start, j + 1
    raise AssertionError(f"{name}: unbalanced braces")


def norm(s):
    return re.sub(r"\s+", "", s)


def pin_of(code, name):
    a, b = func_span(code, name)
    return hashlib.sha256(norm(code[a:b]).encode()).hexdigest()[:16]


def check_header(src, pinned=PINNED):
    code = strip_comments(src)
    if "volatile" in code or re.search(r"\bgoto\b|\bdo\s*\{|__asm|\basm\b", code):
        raise AssertionError("header: volatile / goto / do-while / asm in the pure header")
    if re.search(r"\b0x[0-9A-Fa-f]{5,}", code):
        raise AssertionError("header: a hex literal >= 0x10000 (an address?) in the pure header: constants come from the ADT")
    if code.count("->wr32(") != 1:
        raise AssertionError(f"header: ops->wr32 must be called exactly once, found {code.count('->wr32(')}")
    wa, wb = func_span(code, "sfg_w32")
    w = code[wa:wb]
    if "->wr32(" not in w:
        raise AssertionError("header: the one ops->wr32 call is not inside sfg_w32")
    body = norm(w[w.index("{") + 1:])
    if not re.match(r"conststructsfg_cfg\*c=r->c;uint32_told=0,addr=0;if\(!sfg_reg_writable\(c,off\)\)return-2;", body):
        raise AssertionError("header: sfg_w32 must refuse a non-writable offset as its first statement, before any bus access")
    if body.find("sfg_w32_allowed(") < 0 or body.find("sfg_w32_allowed(") > body.find("->wr32("):
        raise AssertionError("header: sfg_w32 must ask sfg_w32_allowed before the store")
    if code.count("->rd32(") != 2:
        raise AssertionError("header: ops->rd32 only in sfg_r32 and sfg_rx_pop")
    for fn in ("sfg_r32", "sfg_rx_pop"):
        a, b = func_span(code, fn)
        if "->rd32(" not in code[a:b]:
            raise AssertionError(f"header: ops->rd32 missing from {fn}")
    ra, rb = func_span(code, "sfg_r32")
    if not norm(code[ra:rb]).count("if(!sfg_rd_ok(r->c,sp,off))return0;returnr->o->rd32("):
        raise AssertionError("header: sfg_r32 must check sfg_rd_ok before the read")
    if len(re.findall(r"\bsfg_rx_pop\s*\(", code)) != 2:
        raise AssertionError("header: sfg_rx_pop defined once and called once (sfg_xfer)")
    xa, xb = func_span(code, "sfg_xfer")
    if "sfg_rx_pop(" not in code[xa:xb]:
        raise AssertionError("header: sfg_rx_pop only from sfg_xfer")
    for fn in ("sfg_w32_allowed", "sfg_w32", "sfg_reg_writable", "sfg_rd_ok"):
        a, _ = func_span(code, fn)
        if "SFG_NOINL" not in code[max(0, a - 30):a + 40]:
            raise AssertionError(f"header: {fn} must be SFG_NOINL (CR-11: the decision stays one function)")
    for fn, want in pinned.items():
        got = pin_of(code, fn)
        if got != want:
            raise AssertionError(f"header: {fn} is not the audited text (sha {got}, pinned {want})")
    total = len(re.findall(r"\bsfg_w32\s*\(", code)) - 1
    if total != sum(W32_SITES.values()):
        raise AssertionError(f"header: sfg_w32 has exactly {sum(W32_SITES.values())} call sites, found {total}")
    for fn, n in W32_SITES.items():
        a, b = func_span(code, fn)
        if len(re.findall(r"\bsfg_w32\s*\(", code[a:b])) != n:
            raise AssertionError(f"header: {fn} must hold exactly {n} sfg_w32 calls")
    for fn in ("sfg_w32_allowed", "sfg_reg_writable", "sfg_snap_of"):
        if len(re.findall(r"\b" + fn + r"\s*\(", code)) != 2:
            raise AssertionError(f"header: {fn} is defined once and called only by sfg_w32")
    if (REGS_TEXT + chr(10)) not in src or "_Static_assert(SFG_NREGS == 7," not in src or "#define SFG_DEV_ADDR7\t0x36u" not in src:
        raise AssertionError("header: the device allowlist {0x36} and the seven registers are pinned")
    n = norm(code)
    for need in ('_Static_assert(SFG_DEV_ADDR7==0x36u,', "SFG_DEV_ADDR7!=0x69u&&SFG_DEV_ADDR7!=0x66u&&SFG_DEV_ADDR7!=0x25u&&SFG_DEV_ADDR7!=0x57u&&SFG_DEV_ADDR7!=0x35u",
                 "#defineSFG_RANGE(n,r)_Static_assert((r)<=0x4Fu,", "#defineSFG_LEN_PTR1u", "#defineSFG_VAL_BYTES2u",
                 "if(c->addr7!=SFG_DEV_ADDR7)return", "if(c->regs[i]!=sfg_reg_ptr(i))return"):
        if need not in n:
            raise AssertionError(f"header: lost '{need}'")
    # review F5: the failure-time diagnostics only keep and print words the reader already read; they reach no bus access at all
    for fn in F5_NOBUS:
        a, b = func_span(code, fn)
        if re.search(r"\b(?:sfg_r32|sfg_w32|sfg_rx_pop|sfg_poll|sfg_wait|sfg_xfer|sfg_msg_start|sfg_restore)\s*\(|->rd32\(|->wr32\(|->crit\(", code[a:b]):
            raise AssertionError(f"header: F5 diagnostic {fn} must not touch the bus")
    pa, pb = func_span(code, "sfg_poll")
    if len(re.findall(r"\bsfg_r32\s*\(", code[pa:pb])) != 4 or re.search(r"\bsfg_w32\s*\(", code[pa:pb]):
        raise AssertionError("header: sfg_poll reads exactly INT_STATUS, FIFO_STATUS, TRANS_STATUS, ERR_STATUS and writes nothing")
    xa, xb = func_span(code, "sfg_xfer")
    if not norm(code[xa:xb]).startswith("staticintsfg_xfer(conststructsfg_run*r,enumsfg_regreg,uint32_t*raw)"):
        raise AssertionError("header: sfg_xfer takes an enum sfg_reg (no pointer byte or data byte from the caller)")
    loops = len(re.findall(r"\b(?:while|for)\b", code))
    if loops != sum(LOOPS_OK.values()):
        raise AssertionError(f"header: exactly {sum(LOOPS_OK.values())} loops (bounded waits, text helpers, fixed-count loops), found {loops}")
    for fn, n in LOOPS_OK.items():
        a, b = func_span(code, fn)
        if len(re.findall(r"\b(?:while|for)\b", code[a:b])) != n:
            raise AssertionError(f"header: {fn} must hold exactly {n} loops (no retry loop)")
    sa, sb = func_span(code, "sfg_set")
    s = norm(code[sa:sb])
    pos = -1
    for need in SET_ORDER:
        p = s.find(need, pos + 1)
        if p < 0:
            raise AssertionError(f"header: sfg_set lost or reordered '{need}'")
        pos = p
    if s.count("sfg_restore(") != 1 or s.count("st->stopped=1;") != 1 or norm(code).count("st->stopped=1;") != 2:
        raise AssertionError("header: one unconditional restore; every refusal and failure latches the stop")


def check_glue(src):
    code = strip_comments(src)
    if re.search(r"\b0x[0-9A-Fa-f]{5,}", code):
        raise AssertionError("glue: a hex literal >= 0x10000: every address comes from the ADT")
    if re.search(r"\bgoto\b|__asm|\basm\b", code):
        raise AssertionError("glue: goto / asm")
    stores = re.findall(r"\bsb\.(i2c|pad|mode|cmu)\s*\[[^\]]*\]\s*=(?!=)", code)
    if stores != ["i2c"]:
        raise AssertionError(f"glue: exactly one MMIO store, into the HSI2C12 block, found {stores}")
    wa, wb = func_span(code, "sb_wr32")
    if not re.search(r"\bsb\.i2c\s*\[[^\]]*\]\s*=(?!=)", code[wa:wb]):
        raise AssertionError("glue: the one store must be in sb_wr32")
    if re.search(r"\*\s*\(\s*volatile", code) or len(re.findall(r"\bvolatile\b", code)) != 4:
        raise AssertionError("glue: volatile only in the block pointers, sb_rd32 base, sb_map (no cast-and-deref)")
    if len(re.findall(r"\bsb_wr32\b", code)) != 2 or "sb.ops.wr32=sb_wr32;" not in norm(code):
        raise AssertionError("glue: sb_wr32 is only the ops.wr32 hook")
    ia, ib = func_span(code, "sweet_batt_init")
    i = norm(code[ia:ib])
    order = ['if(!PE_parse_boot_argn("sweetbatt",&on,sizeof(on))||on==0){return;}', "SecureDTLookupEntry(", "why=sfg_cfg_load(&sb.cfg,&props);",
             "sb.i2c=sb_map(", "kernel_thread_start("]
    pos = -1
    for need in order:
        p = i.find(need)   # the FIRST occurrence: nothing of a later step may come before an earlier one
        if p <= pos:
            raise AssertionError(f"glue: sweet_batt_init lost or reordered '{need}' (no ADT read and no map before the boot-arg)")
        pos = p
    if norm(code).count("sb_map(") != 5:
        raise AssertionError("glue: sb_map defined once and called for the four ADT blocks only")
    nostr = re.sub(r'"(?:\\.|[^"\\])*"', '""', code)   # string literals out ("ml_io_map(%#llx) failed")
    ma, mb = func_span(nostr, "sb_map")
    if len(re.findall(r"\bml_io_map\s*\(", nostr)) != 2 or len(re.findall(r"\bml_io_map\s*\(", nostr[ma:mb])) != 1:   # the extern + the call in sb_map
        raise AssertionError("glue: ml_io_map only inside sb_map")


def load():
    text = open(PATCH).read()
    return new_file(text, "bsd/dev/sweet_hsi2c_fg.h"), new_file(text, "bsd/dev/sweet_hsi2c.c"), text


HDR_MUTANTS = [
    ("second store", "\treturn r->o->wr32(r->o->ctx, off, v) ? 0 : -1;\n}", "\treturn r->o->wr32(r->o->ctx, off, v) ? 0 : -1;\n}\nstatic void\nsfg_evil(const struct sfg_run *r)\n{\n\tr->o->wr32(r->o->ctx, 0x40u, 0);\n}"),
    ("writable check moved", "\tif (!sfg_reg_writable(c, off))\n\t\treturn -2;\t\t\t/* not one of the seven registers: not even read */\n", ""),
    ("allowlist value widened", "return v == c->v_int_en || v == rv;", "return 1;"),
    ("a writable CONF", "off == c->o_tx || off == c->o_auto ||\n\t       off == c->o_addr;", "off == c->o_tx || off == c->o_auto ||\n\t       off == c->o_addr || off == c->o_conf;"),
    ("another device", "c->v_addr = SFG_DEV_ADDR7 << c->f_mas_shift;", "c->v_addr = 0x69u << c->f_mas_shift;"),
    ("an eighth register", "X(DEVNAME, 0x21u)", "X(DEVNAME, 0x21u) X(MODEL, 0x80u)"),
    ("device constant", "#define SFG_DEV_ADDR7\t0x36u", "#define SFG_DEV_ADDR7\t0x69u"),
    ("forbidden assert dropped", "SFG_DEV_ADDR7 != 0x69u && ", ""),
    ("not noinline", "static SFG_NOINL int\nsfg_w32_allowed(", "static int\nsfg_w32_allowed("),
    ("MMIO in the header", "#define SFG_BIT(n)", "#define SFG_EVIL 0x10D50000u\n#define SFG_BIT(n)"),
    ("rd without the read allowlist", "\tif (!sfg_rd_ok(r->c, sp, off))\n\t\treturn 0;\n\treturn r->o->rd32", "\treturn r->o->rd32"),
    ("an extra w32 site", "\te |= sfg_w32(r, c->o_int_en, s->inten);", "\te |= sfg_w32(r, c->o_int_en, s->inten);\n\te |= sfg_w32(r, c->o_int_en, s->inten);"),
    ("restore drops ADDR", "\te |= sfg_w32(r, c->o_addr, s->addr);", ""),
    ("a retry loop", "\twhy = sfg_gate(&r);", "\twhile (sfg_gate(&r))\n\t\t;\n\twhy = sfg_gate(&r);"),
    ("no stop latch", "\tif (st->stopped)\n\t\treturn 1;\n\tr.o = o;", "\tr.o = o;"),
    ("gate after the controller reads", "\twhy = sfg_gate(&r);\n\tif (why)\n\t\treturn sfg_refuse(st, o, why);\n", ""),
    ("restore only on success", "restored = sfg_restore(&r, first || e != SFG_OK);", "restored = e == SFG_OK ? sfg_restore(&r, first) : 1;"),
    ("a data byte from the caller", "sfg_xfer(const struct sfg_run *r, enum sfg_reg reg, uint32_t *raw)", "sfg_xfer(const struct sfg_run *r, uint32_t reg, uint32_t *raw)"),
    ("F5 diag reads a register", "\tuint64_t dt = r->o->now(r->o->ctx) - t0;\n", "\tuint64_t dt = r->o->now(r->o->ctx) - t0;\n\tuint32_t x = 0;\n\n\t(void)sfg_r32(r, SFG_SP_I2C, r->c->o_ctl, &x);\n"),
    ("F5 diag prints with a bus write", "\tsfg_txt(&b, \" ERR=\");\n", "\t(void)sfg_w32(0, 0u, 0u);\n\tsfg_txt(&b, \" ERR=\");\n"),
    ("a fifth poll read", "sfg_r32(r, SFG_SP_I2C, c->o_err, &err);\n", "sfg_r32(r, SFG_SP_I2C, c->o_err, &err) && sfg_r32(r, SFG_SP_I2C, c->o_conf, &err);\n"),
    ("a goto", "\tif (st->stopped)\n\t\treturn 1;", "\tif (st->stopped)\n\t\tgoto out;\nout:"),
]
GLUE_MUTANTS = [
    ("second store", "\t*v = base[off / 4u];", "\t*v = base[off / 4u];\n\tsb.cmu[0] = 0;"),
    ("store moved", "\tsb.i2c[off / 4u] = v;\n", ""),
    ("an address typed in", '#define SB_COMPAT', "#define SB_EVIL 0x10D50000ull\n#define SB_COMPAT"),
    ("map before the boot-arg", "\tstarted = 1;\n", "\tstarted = 1;\n\t(void)sb_map(0, 4);\n"),
    ("ADT before the boot-arg", "\tstarted = 1;\n", "\tstarted = 1;\n\t(void)SecureDTLookupEntry(NULL, \"/sweet-batt\", &node);\n"),
    ("boot-arg default on", "|| on == 0) {", ") {"),
    ("raw pointer store", "\tsb.i2c[off / 4u] = v;", "\t*(volatile uint32_t *)(sb.i2c + off / 4u) = v;"),
    ("direct ml_io_map", "\tsb.cmu = sb_map(sb.cfg.cmu_pa, sb.cfg.cmu_len);", "\tsb.cmu = (volatile uint32_t *)ml_io_map(0, 4);"),
]


def fill_pins(hdr):
    code = strip_comments(hdr)
    return {fn: pin_of(code, fn) for fn in PINNED}


class Hsi2cFgAudit(unittest.TestCase):
    def test_the_patch_passes(self):
        hdr, glue, _ = load()
        check_header(hdr)
        check_glue(glue)

    def test_header_mutants_are_killed(self):
        hdr, _, _ = load()
        check_header(hdr)
        for what, a, b in HDR_MUTANTS:
            if hdr.count(a) != 1:
                self.fail(f"mutant '{what}': pattern not found exactly once (update the mutant)")
            with self.assertRaises(AssertionError, msg=what):
                check_header(hdr.replace(a, b))

    def test_glue_mutants_are_killed(self):
        _, glue, _ = load()
        check_glue(glue)
        for what, a, b in GLUE_MUTANTS:
            if glue.count(a) != 1:
                self.fail(f"mutant '{what}': pattern not found exactly once (update the mutant)")
            with self.assertRaises(AssertionError, msg=what):
                check_glue(glue.replace(a, b))

    def test_pins_are_complete(self):
        for fn, v in PINNED.items():
            self.assertRegex(v, r"^[0-9a-f]{16}$", fn)

    def test_hooked_once_and_registered(self):
        _, _, text = load()
        self.assertEqual(text.count("+\tsweet_batt_init();"), 1)
        self.assertEqual(text.count("+bsd/dev/sweet_hsi2c.c\t\t\tstandard"), 1)
        # every added line of the patch is in one of the four files
        files = re.findall(r"(?m)^diff --git a/(\S+) b/", text)
        self.assertEqual(sorted(files), ["bsd/conf/files", "bsd/dev/sweet_hsi2c.c", "bsd/dev/sweet_hsi2c_fg.h", "bsd/dev/sweet_ufs.c"])
        known = open(os.path.join(ROOT, "tools", "bootargs-known.txt")).read()
        self.assertIn("sweetbatt\tpatch:0148\n", known)


if __name__ == "__main__":
    unittest.main()
