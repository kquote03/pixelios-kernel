#!/usr/bin/env python3
"""Tests for tools/phone-battery (TP-031) with a fake fastboot."""
import json, os, stat, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "phone-battery")
FAKE = r'''#!/usr/bin/env python3
import os, sys, time
a = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(" ".join(a) + "\n")
if os.environ.get("FAKE_HANG"):
    time.sleep(30)
v = {"product": os.environ.get("FAKE_PRODUCT", "oriole"), "battery-voltage": os.environ.get("FAKE_MV", "4123"),
     "battery-soc-ok": "yes"}
if a[:1] == ["getvar"] and a[1] in v and v[a[1]]:
    sys.stderr.write("%s: %s\nFinished. Total time: 0.001s\n" % (a[1], v[a[1]])); sys.exit(0)
if a[:1] == ["getvar"]:
    sys.stderr.write("getvar:%s FAILED (remote: 'GetVar Variable Not found')\n" % a[1]); sys.exit(1)
sys.exit(3)
'''


class T(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        d = self.t.name
        self.fb = os.path.join(d, "fastboot")
        with open(self.fb, "w") as f:
            f.write(FAKE)
        os.chmod(self.fb, os.stat(self.fb).st_mode | stat.S_IEXEC)
        self.log = os.path.join(d, "fb.log")
        self.env = dict(os.environ, PB_FASTBOOT=self.fb, FAKE_LOG=self.log, BOARD="oriole", PB_TIMEOUT="3")

    def tearDown(self):
        self.t.cleanup()

    def run_tool(self, *a, **env):
        p = subprocess.run([sys.executable, TOOL, *a], capture_output=True, text=True, env=dict(self.env, **env), timeout=60)
        return p.returncode, p.stdout

    def sent(self):
        if not os.path.exists(self.log):
            return []
        with open(self.log) as f:
            return f.read().splitlines()

    def test_pass(self):
        rc, out = self.run_tool()
        self.assertEqual(rc, 0, out)
        self.assertIn("RESULT: PASS battery-voltage=4123 mV soc-ok=yes", out)
        for line in self.sent():
            self.assertTrue(line.startswith("getvar "), line)

    def test_wrong_product(self):
        rc, out = self.run_tool(FAKE_PRODUCT="bluejay")
        self.assertEqual(rc, 2, out)
        self.assertEqual(self.sent(), ["getvar product"])

    def test_missing_var_is_fail(self):
        rc, out = self.run_tool(FAKE_MV="")
        self.assertEqual(rc, 1, out)
        self.assertIn("RESULT: FAIL", out)

    def test_hang_is_error(self):
        rc, out = self.run_tool(FAKE_HANG="1")
        self.assertEqual(rc, 2, out)

    def test_record(self):
        rd = os.path.join(self.t.name, "run")
        os.makedirs(rd)
        with open(os.path.join(rd, "run.json"), "w") as f:
            json.dump({"label": "x"}, f)
        rc, out = self.run_tool("--record", rd)
        self.assertEqual(rc, 0, out)
        with open(os.path.join(rd, "run.json")) as f:
            j = json.load(f)
        self.assertEqual(j["label"], "x")
        self.assertEqual(j["battery_mv"], 4123)
        self.assertEqual(j["battery_soc_ok"], "yes")

    def test_min(self):
        rc, out = self.run_tool("--min", "4200")
        self.assertEqual(rc, 1, out)
        self.assertIn("below 4200", out)


if __name__ == "__main__":
    unittest.main()
