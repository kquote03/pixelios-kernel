"""tools/build-xnu-image.sh --wlan-level: 1..6 (patches 0124/0129-0132: sweetwlan=N), only with --wlan-hold; levels >= 4 copy the image and need --wlanfw."""
import os, subprocess, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(*a):
    env = dict(os.environ, XNU_IMAGE_ARGS_ONLY="1", BOARD="oriole")
    p = subprocess.run(["bash", "tools/build-xnu-image.sh", "--board", "oriole", *a], cwd=ROOT, capture_output=True, text=True, env=env)
    return p.returncode, p.stdout + p.stderr


class WlanLevel(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.NamedTemporaryFile()

    def tearDown(self):
        self.t.close()

    def test_levels_1_to_6(self):
        for n in range(1, 7):
            extra = ["--wlanfw", self.t.name] if n >= 4 else []
            rc, out = run("--wlan-hold", "--wlan-level", str(n), *extra)
            self.assertEqual(rc, 0, out)
            self.assertIn(" sweetwlan=%d" % n, out)
            self.assertEqual(out.count("sweetwlan="), 1, out)

    def test_bad_levels_refused(self):
        for n in ("0", "7", "x", "3a"):
            rc, out = run("--wlan-hold", "--wlan-level", n)
            self.assertEqual(rc, 2, n + out)

    def test_level_needs_hold(self):
        rc, out = run("--wlan-level", "3")
        self.assertEqual(rc, 2, out)

    def test_image_levels_need_firmware(self):
        for n in ("4", "5", "6"):
            rc, out = run("--wlan-hold", "--wlan-level", n)
            self.assertEqual(rc, 2, out)
            self.assertIn("--wlanfw", out)


if __name__ == "__main__":
    unittest.main()
