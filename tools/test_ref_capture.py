#!/usr/bin/env python3
"""Tests for tools/ref-capture (TP-056) with a fake adb: device/state refusal, the allowlist/denylist, timeouts, output files."""
import ast, importlib.machinery, importlib.util, os, re, stat, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "ref-capture")

FAKE = r'''#!/usr/bin/env python3
import os, sys, time
a = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(repr(a) + "\n")
if a[:1] == ["get-state"]:
    print(os.environ.get("FAKE_STATE", "recovery")); sys.exit(0)
if a[:2] == ["shell", "getprop"]:
    print(os.environ.get("FAKE_DEV", "oriole")); sys.exit(0)
if a[:1] == ["exec-out"]:
    cmd = a[1]
    hang = os.environ.get("FAKE_HANG")
    if hang and hang in cmd:
        time.sleep(30)
    if "/proc/interrupts" in cmd and os.environ.get("FAKE_EMPTY_IRQ"):
        sys.exit(0)
    if "by-name/misc" in cmd:
        sys.stdout.buffer.write(b"bootonce-bootloader\x00" + b"\x00" * 100 + b"recovery\n--wipe\x00"); sys.exit(0)
    sys.stdout.write("out of " + cmd[:40] + "\n"); sys.exit(0)
sys.exit(3)
'''


def load():
    loader = importlib.machinery.SourceFileLoader("ref_capture", TOOL)
    spec = importlib.util.spec_from_loader("ref_capture", loader)
    m = importlib.util.module_from_spec(spec)
    loader.exec_module(m)
    return m


class T(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        d = self.t.name
        self.adb = os.path.join(d, "adb")
        with open(self.adb, "w") as f:
            f.write(FAKE)
        os.chmod(self.adb, os.stat(self.adb).st_mode | stat.S_IEXEC)
        self.log = os.path.join(d, "adb.log")
        self.out = os.path.join(d, "out")
        self.env = dict(os.environ, REF_ADB=self.adb, FAKE_LOG=self.log, BOARD="oriole", REF_HOST_TIMEOUT="3")

    def tearDown(self):
        self.t.cleanup()

    def run_tool(self, *a, **env):
        e = dict(self.env, **env)
        p = subprocess.run([sys.executable, TOOL, "--out", self.out, *a], capture_output=True, text=True, env=e, timeout=120)
        return p.returncode, p.stdout

    def sent(self):
        if not os.path.exists(self.log):
            return []
        with open(self.log) as f:
            return [ast.literal_eval(l) for l in f]

    def test_wrong_device_refused_before_any_read(self):
        rc, out = self.run_tool(FAKE_DEV="bluejay")
        self.assertEqual(rc, 2, out)
        self.assertIn("RESULT: ERROR", out)
        self.assertFalse([a for a in self.sent() if a[:1] == ["exec-out"]])

    def test_default_board_is_bluejay(self):
        e = dict(self.env); e.pop("BOARD")
        p = subprocess.run([sys.executable, TOOL, "--out", self.out], capture_output=True, text=True, env=e, timeout=60)
        self.assertEqual(p.returncode, 2, p.stdout)
        self.assertFalse([a for a in self.sent() if a[:1] == ["exec-out"]])

    def test_not_recovery_refused(self):
        rc, out = self.run_tool(FAKE_STATE="device")
        self.assertEqual(rc, 2, out)
        self.assertIn("not in recovery", out)
        self.assertFalse([a for a in self.sent() if a[:1] == ["exec-out"]])

    def test_happy_path(self):
        rc, out = self.run_tool()
        self.assertEqual(rc, 0, out)
        m = re.search(r"RESULT: PASS files=(\d+) dir=(\S+)", out)
        self.assertTrue(m, out)
        mod = load()
        self.assertEqual(int(m.group(1)), len(mod.ITEMS))
        self.assertTrue(os.path.isdir(m.group(2)))
        for name, fname, _ in mod.ITEMS:
            self.assertTrue(os.path.getsize(os.path.join(m.group(2), fname)) > 0, fname)
        # misc BCB: strings extracted on the host
        txt = open(os.path.join(m.group(2), "misc-bcb.txt")).read()
        self.assertIn("bootonce-bootloader", txt)
        self.assertIn("recovery", txt)
        for a in self.sent():
            if a[:1] == ["exec-out"]:
                self.assertIsNone(mod.refusal(a[1]), a)
                self.assertIn("timeout 8", a[1])

    def test_empty_item_is_reported(self):
        rc, out = self.run_tool(FAKE_EMPTY_IRQ="1")
        self.assertIn("EMPTY interrupts", out)
        self.assertEqual(rc, 0, out)

    def test_hang_is_timeout_and_fail_but_others_captured(self):
        rc, out = self.run_tool(FAKE_HANG="/proc/iomem")
        self.assertEqual(rc, 1, out)
        self.assertIn("TIMEOUT iomem", out)
        self.assertIn("OK dmesg", out)

    def test_only_filter(self):
        rc, out = self.run_tool("--only", "dmesg,gpio")
        self.assertEqual(rc, 0, out)
        self.assertIn("files=2", out)
        rc, out = self.run_tool("--only", "nosuch")
        self.assertEqual(rc, 2, out)

    def test_denylist(self):
        mod = load()
        for bad in ["timeout 8 cat /sys/kernel/debug/clk/clk_summary", "cat /sys/kernel/debug/regmap/spi13.0/registers",
                    "cat /sys/kernel/debug/regmap/*/registers", "echo 1 > /sys/class/regulator/x/state", "dd if=/dev/zero of=/dev/block/sda",
                    "timeout 8 tee /x", "insmod /x.ko", "reboot bootloader", "setprop a b", "cat /proc/x >> /y", "rm -f /x",
                    "mount -o remount,rw /", "ls /sys/kernel/debug/clk"]:
            self.assertIsNotNone(mod.refusal(bad), bad)
        for name, fname, cmd in mod.ITEMS:
            self.assertIsNone(mod.refusal(cmd), cmd)

    def test_list(self):
        p = subprocess.run([sys.executable, TOOL, "--list"], capture_output=True, text=True, env=self.env, timeout=30)
        self.assertEqual(p.returncode, 0)
        self.assertIn("dmesg", p.stdout)
        self.assertFalse(self.sent())


if __name__ == "__main__":
    unittest.main()
