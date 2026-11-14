"""The /sweet-batt ADT node (tools/mkadt.py) of xnu patch 0148 (bsd/dev/sweet_hsi2c_fg.h, boot-arg sweetbatt=1): every value equals soc.json
hsi2c / peri and board.json fg, typed here independently; the device allowlist {0x36} and the seven registers are pinned and anything else
in board.json is refused; only boards with an fg block get the node; the property names are exactly the ones the patch header reads."""
import copy
import os
import re
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import adt, boards, mkadt  # noqa: E402

PATCH = os.path.join(ROOT, "xnu", "patches", "0148-sweet-hsi2c-fg.patch")


def node_props(board, **kw):
    root = adt.parse(adt.serialize(mkadt.build(seed=b"\0" * 64, board=board, **kw)))[0]
    for c in root.children:
        props = dict((k, v) for k, _f, v in c.props)
        if props["name"] == b"sweet-batt\0":
            return props
    return None


def u32s(b):
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


class SweetBattNode(unittest.TestCase):
    def test_only_boards_with_a_fuel_gauge(self):
        self.assertIsNotNone(node_props("oriole"))
        self.assertIsNone(node_props("bluejay"))   # bluejay's gauge wiring is not checked (no fg block)
        self.assertIsNone(node_props("sweet"))
        self.assertIsNone(node_props("oriole", qemu=True))

    def test_values_equal_the_cited_tables(self):
        p = node_props("oriole")
        # independent expectations: soc.json hsi2c/peri (cites in notes.hsi2c / notes.peri), board.json fg (notes.fg)
        self.assertEqual(p["compatible"], b"google,gs101-hsi2c-fg\0")
        self.assertEqual(struct.unpack("<2Q", p["reg"]), (0x10D50000, 0xC8))          # hsi2c_12; extent = USI_CON 0xC4 + 4
        self.assertEqual(struct.unpack("<2Q", p["pad-bank"]), (0x10C40060, 0x8))      # gpp23 bank, CON +0 / DAT +4
        self.assertEqual(struct.unpack("<2Q", p["usi-mode-reg"]), (0x10C21010, 0x4))  # sysreg_peric1 + 0x10
        self.assertEqual(struct.unpack("<2Q", p["cmu-reg"]), (0x10C00000, 0x3034))    # CMU_PERIC1, extent = QCH 0x3030 + 4
        want = {"usi-mode-val": 0x4, "usi-mode-mask": 0x7, "cmu-gate-ipclk-off": 0x2030, "cmu-gate-pclk-off": 0x2050, "cmu-qch-off": 0x3030,
                "cmu-cg-val-bit": 21, "cmu-qch-enable-bit": 0, "cmu-qch-req-bit": 1, "pad-con-off": 0x0, "pad-dat-off": 0x4, "pin-scl": 4, "pin-sda": 5,
                "off-ctl": 0x00, "off-fifo-ctl": 0x04, "off-trailing-ctl": 0x08, "off-int-enable": 0x20, "off-int-status": 0x24, "off-err-status": 0x2C,
                "off-fifo-status": 0x30, "off-tx-data": 0x34, "off-rx-data": 0x38, "off-conf": 0x40, "off-auto-conf": 0x44, "off-timeout": 0x48,
                "off-trans-status": 0x50, "off-addr": 0x70, "off-usi-con": 0xC4,
                "bit-ctl-master": 3, "bit-ctl-rxchon": 6, "bit-ctl-txchon": 7, "bit-fifo-rxfifo-en": 0, "bit-fifo-txfifo-en": 1, "bit-int-tx-underrun": 2,
                "bit-int-tx-overrun": 3, "bit-int-rx-underrun": 4, "bit-int-rx-overrun": 5, "bit-int-trans-done": 7, "bit-int-trans-abort": 8,
                "bit-int-no-dev-ack": 9, "bit-int-no-dev": 10, "bit-int-timeout": 11, "bit-fifo-tx-full": 7, "bit-fifo-tx-empty": 8, "bit-fifo-rx-full": 23,
                "bit-fifo-rx-empty": 24, "bit-conf-hs-mode": 29, "bit-conf-10bit-addr": 30, "bit-conf-auto-mode": 31, "bit-auto-read-write": 16,
                "bit-auto-stop-after-trans": 17, "bit-auto-master-run": 31, "bit-timeout-en": 31, "bit-trans-slave-busy": 16, "bit-trans-master-busy": 17,
                "bit-usi-con-reset": 0,
                "fld-fifo-rx-trig-shift": 4, "fld-fifo-tx-trig-shift": 16, "fld-fifo-rx-lvl-shift": 16, "fld-fifo-lvl-mask": 0x7F, "fld-auto-len-mask": 0xFFFF,
                "fld-addr-mas-shift": 10, "fld-addr-field-mask": 0x3FF, "fld-trans-st-mask": 0xF, "fld-st-idle": 0x0, "fld-st-init": 0x1, "fld-st-no-ack": 0xB,
                "fld-st-lose": 0xC, "fld-trailing-count": 0xFFFFFF, "fld-tx-fifo-depth": 16, "fld-pad-func": 0x2,
                "fg-addr7": 0x36, "fg-vcell-nv-per-lsb": 78125, "fg-repsoc-lsb-per-pct": 256, "fg-temp-lsb-per-c": 256,
                "fg-curr-na-per-lsb": 312500,   # 156250 * 1000 / rsense 500 (10 uOhm units): max1720x_battery.c:473-477
                "handoff-ctl": 0x48, "handoff-fifo-ctl": 0x00080083, "handoff-trailing-ctl": 0x00FFFFFF, "handoff-int-enable": 0x80,
                "handoff-conf": 0x980110FF, "handoff-auto-conf": 0x00030001, "handoff-timeout": 0xFF, "handoff-addr": 0x9400, "handoff-usi-con": 0}
        for k, v in want.items():
            self.assertEqual(u32s(p[k]), [v], k)
        self.assertEqual(u32s(p["fg-regs"]), [0x06, 0x08, 0x09, 0x0A, 0x0B, 0x35, 0x21])
        extra = set(p) - set(want) - {"name", "AAPL,phandle", "compatible", "reg", "pad-bank", "usi-mode-reg", "cmu-reg", "fg-regs"}
        self.assertEqual(extra, set(), "no property without an independent expectation")

    def test_names_fit_the_adt(self):
        props = mkadt.sweet_batt_props(boards.BOARDS["oriole"])
        names = [k for k, _v in props]
        self.assertEqual(len(names), len(set(names)))
        for k in names:
            self.assertLessEqual(len(k), 31, k)

    def test_c_table_is_the_node_bytes(self):
        # shim/build.sh feeds this table to the host test: it must hold exactly the node's bytes
        c = mkadt.sweet_batt_c("oriole")
        rows = re.findall(r'\{ "([\w,-]+)", (\d+), \{ ([0-9a-fx, ]+) \} \},', c)
        got = {k: bytes(int(b, 16) for b in v.split(", ")) for k, n, v in rows}
        self.assertEqual(len(rows), len(mkadt.sweet_batt_props(boards.BOARDS["oriole"])))
        self.assertEqual(got, {k: v for k, v in node_props("oriole").items() if k not in ("name", "AAPL,phandle")})
        for k, n, v in rows:
            self.assertEqual(int(n), len(got[k]))
            self.assertLessEqual(int(n), 64)
        with self.assertRaises(ValueError):
            mkadt.sweet_batt_c("bluejay")

    def test_the_allowlist_is_pinned(self):
        self.assertEqual(mkadt.FG_ADDR7, 0x36)
        self.assertEqual(mkadt.FG_REGS, (("REPSOC", 0x06), ("TEMP", 0x08), ("VCELL", 0x09), ("CURRENT", 0x0A), ("AVGCURRENT", 0x0B),
                                         ("FULLCAPREP", 0x35), ("DEVNAME", 0x21)))

    def bad(self, mutate, msg):
        cfg = copy.deepcopy(boards.BOARDS["oriole"])
        mutate(cfg)
        with self.assertRaises(ValueError, msg=msg):
            mkadt.sweet_batt_props(cfg)

    def test_board_json_outside_the_allowlist_is_refused(self):
        for a in ("0x69", "0x66", "0x25", "0x57", "0x35", "0x37", "0x00"):
            self.bad(lambda c, a=a: c["fg"].__setitem__("addr7", a), f"addr7 {a}")
        self.bad(lambda c: c["fg"]["regs"].append(["MODEL", "0x80"]), "an eighth register")
        self.bad(lambda c: c["fg"]["regs"].pop(), "a register missing")
        self.bad(lambda c: c["fg"]["regs"].reverse(), "another order")
        self.bad(lambda c: c["fg"]["regs"].__setitem__(0, ["REPSOC", "0x60"]), "the model-unlock register")
        self.bad(lambda c: c["fg"].__setitem__("bus", "spi13"), "another bus")
        self.bad(lambda c: c["fg"].__setitem__("rsense_10uohm", 0), "rsense 0")
        self.bad(lambda c: c["fg"].__setitem__("rsense_10uohm", 3), "rsense not dividing")
        self.bad(lambda c: c["fg"]["handoff"].pop("conf"), "a handoff word missing")
        self.bad(lambda c: c["fg"]["handoff"].__setitem__("addr", "0x9401"), "handoff ADDR outside the master field")
        self.bad(lambda c: c["fg"]["handoff"].__setitem__("auto_conf", "0x80030001"), "handoff AUTO_CONF with MASTER_RUN")
        self.bad(lambda c: c["hsi2c"]["regs_off"].__setitem__("conf", "0x44"), "hsi2c differs from peri.regs.i2c")
        self.bad(lambda c: c["hsi2c"]["regs_off"].__setitem__("tx_data", "0x3C"), "tx_data differs from peri.fifo_offs")
        self.bad(lambda c: c["hsi2c"]["bits"].__setitem__("ctl_master", 32), "a bit number 32")
        self.bad(lambda c: c["hsi2c"]["bits"].pop("auto_master_run"), "a bit missing")
        self.bad(lambda c: c["peri"]["pins"].__setitem__(1, ["I2C12_SDA", "gpp25", 5, "i2c12"]), "the two pads on different banks")

    def test_property_names_equal_the_patch_header(self):
        if not os.path.exists(PATCH):
            self.fail("xnu/patches/0148-sweet-hsi2c-fg.patch missing")
        text = open(PATCH).read()
        hdr = []
        on = False
        for line in text.splitlines():
            if line == "+++ b/bsd/dev/sweet_hsi2c_fg.h":
                on = True
                continue
            if on and line.startswith("diff --git "):
                break
            if on and line.startswith("+"):
                hdr.append(line[1:])
        hdr = re.sub(r"/\*.*?\*/", " ", "\n".join(hdr), flags=re.S)
        u32 =set(re.findall(r'X\(\s*[\w\[\]]+\s*,\s*"([\w-]+)"\s*,\s*\w+\s*\)', hdr))
        pa = set(re.findall(r'X\(\s*\w+\s*,\s*\w+\s*,\s*"([\w-]+)"\s*\)', hdr))
        if not u32 or not pa:
            self.fail("the header's SFG_CFG_U32 / SFG_CFG_PA lists not found")
        names = {k for k, _v in mkadt.sweet_batt_props(boards.BOARDS["oriole"])} - {"compatible"}
        self.assertEqual(u32 | pa, names)


if __name__ == "__main__":
    unittest.main()
