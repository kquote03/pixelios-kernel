"""tools/mk_launchd_job.py (G5 self-test run): the replacement /System/Library/xpc/launchd.plist is the root's own cache plus exactly one job.
Run: python3 -B tools/test_mk_launchd_job.py"""
import hashlib, os, plistlib, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "mk_launchd_job.py")
KEY = "/System/Library/LaunchDaemons/org.pixelios.sweetgl-selftest.plist"


def base():
    return {"AppExtensions": {"a": {"x": 1}}, "AppRemovalServices": {}, "Symlinks": {"s": "t"}, "SystemLibraryTreeState": {"p": b"\x01\x02"}, "VersionNumber": 7,
            "LaunchDaemons": {"/System/Library/LaunchDaemons/com.apple.backboardd.plist": {"Label": "com.apple.backboardd", "ProgramArguments": ["/usr/libexec/backboardd"], "KeepAlive": True},
                              "/Library/LaunchDaemons/x.plist": {"Label": "x", "Program": "/bin/x", "MachServices": {"x": True}}}}


class T(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.i, self.o = os.path.join(self.d, "in.plist"), os.path.join(self.d, "out.plist")

    def put(self, obj, fmt=plistlib.FMT_BINARY):
        with open(self.i, "wb") as fh:
            plistlib.dump(obj, fh, fmt=fmt)
        return hashlib.sha256(open(self.i, "rb").read()).hexdigest()

    def run_tool(self, *a):
        return subprocess.run([sys.executable, "-I", TOOL, self.i, self.o] + list(a), capture_output=True, text=True)

    def test_adds_exactly_one_job(self):
        h = self.put(base())
        r = self.run_tool("--sha256", h)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = plistlib.load(open(self.o, "rb"))
        want = base()
        job = out["LaunchDaemons"].pop(KEY)
        self.assertEqual(out, want)                                     # everything else is untouched, in every section
        self.assertEqual(job, {"Label": "org.pixelios.sweetgl-selftest", "RunAtLoad": True,
                               "ProgramArguments": ["/usr/local/bin/sweetgl_once", "/usr/local/bin/sweetgl_selftest", "/usr/local/lib/libsweetgl.dylib"]})
        self.assertEqual(open(self.o, "rb").read(8), b"bplist00")
        self.assertIn("jobs 2 -> 3", r.stdout)

    def test_sweetaudio_job(self):
        h = self.put(base())
        r = self.run_tool("--sha256", h, "--job", "sweetaudio")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = plistlib.load(open(self.o, "rb"))
        job = out["LaunchDaemons"].pop("/System/Library/LaunchDaemons/org.pixelios.sweetaudio.plist")
        self.assertEqual(out, base())
        self.assertEqual(job, {"Label": "org.pixelios.sweetaudio", "RunAtLoad": True, "ProgramArguments": ["/usr/local/bin/sweetaudio_install"]})
        self.assertEqual(self.run_tool("--sha256", h, "--job", "nope").returncode, 2)

    def test_deterministic(self):
        h = self.put(base())
        self.assertEqual(self.run_tool("--sha256", h).returncode, 0)
        a = open(self.o, "rb").read()
        self.assertEqual(self.run_tool("--sha256", h).returncode, 0)
        self.assertEqual(a, open(self.o, "rb").read())

    def test_refuses_wrong_input_hash(self):
        self.put(base())
        r = self.run_tool("--sha256", "0" * 64)
        self.assertEqual(r.returncode, 2)
        self.assertFalse(os.path.exists(self.o))
        self.assertEqual(self.run_tool().returncode, 2)                  # the hash is not optional

    def test_refuses_existing_job_or_label(self):
        b = base(); b["LaunchDaemons"][KEY] = {"Label": "other"}
        self.assertEqual(self.run_tool("--sha256", self.put(b)).returncode, 2)
        b = base(); b["LaunchDaemons"]["/Library/LaunchDaemons/y.plist"] = {"Label": "org.pixelios.sweetgl-selftest"}
        self.assertEqual(self.run_tool("--sha256", self.put(b)).returncode, 2)
        self.assertFalse(os.path.exists(self.o))

    def test_refuses_unexpected_shape(self):
        for bad in ({"LaunchDaemons": []}, {"VersionNumber": 7}, [1, 2], {"LaunchDaemons": {}, "VersionNumber": 8}):
            r = self.run_tool("--sha256", self.put(bad))
            self.assertEqual(r.returncode, 2, bad)
        with open(self.i, "wb") as fh:
            fh.write(b"not a plist")
        self.assertEqual(self.run_tool("--sha256", hashlib.sha256(b"not a plist").hexdigest()).returncode, 2)

    def test_real_cache_if_present(self):
        p = os.path.expanduser("~/pixelios-artifacts/gpu/g5/root-ro/launchd.plist")
        if not os.path.exists(p):
            self.skipTest("no extracted cache on this host")
        self.i = p
        r = self.run_tool("--sha256", "7aab4ecbdaec394205a955f0dce2abf9dd571614b3aee53eaa7439d455763ede")
        self.assertEqual(r.returncode, 0, r.stderr)
        a, b = plistlib.load(open(p, "rb")), plistlib.load(open(self.o, "rb"))
        self.assertEqual(len(b["LaunchDaemons"]), len(a["LaunchDaemons"]) + 1)
        del b["LaunchDaemons"][KEY]
        self.assertEqual(a, b)


if __name__ == "__main__":
    unittest.main()
