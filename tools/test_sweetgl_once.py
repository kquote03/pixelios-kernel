"""gpu/sweetgl-once/sweetgl_once.c (G5 launcher for sweetgl_selftest): built for the host with gcc (+ASan/UBSan) and driven with fake self-tests.

Rules under test (docs/research/52 s.12):
1. A try whose last line is 'sweetgl selftest: clear=SKIP tri=SKIP' and that printed no 'GL_RENDERER=' line ran no GPU work: retry after the delay.
2. Any other outcome (a PASS/FAIL, a renderer line, a missing result line, a crash) ends the retries: the GPU test runs at most once.
3. At most MAX tries; every try is bounded in time (the child is killed) and in output bytes; the exit status is the child's (0 only for a PASS).
4. Every child line is forwarded to the log sink (here a file, on the phone /dev/null, tee'd by xnu patch 0048).
Run: python3 -B tools/test_sweetgl_once.py"""
import os, subprocess, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "gpu", "sweetgl-once", "sweetgl_once.c")


class Once(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.d = tempfile.mkdtemp()
        cls.exe = os.path.join(cls.d, "once")
        subprocess.run(["gcc", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-DSWEETGL_ONCE_TEST", "-o", cls.exe, SRC], check=True)

    def fake(self, body):
        p = os.path.join(self.d, "fake%d.sh" % abs(hash(body)))
        with open(p, "w") as fh:
            fh.write("#!/bin/sh\n" + body)
        os.chmod(p, 0o755)
        return p

    def run_once(self, body, state=None, extra_env=None):
        log = os.path.join(self.d, "log.txt")
        if os.path.exists(log):
            os.unlink(log)
        st = state or os.path.join(self.d, "count")
        if os.path.exists(st):
            os.unlink(st)
        env = dict(os.environ, SWEETGL_ONCE_LOG=log, SWEETGL_ONCE_DELAY_MS="10", SWEETGL_ONCE_TRY_MS="2000", STATE=st)
        env.update(extra_env or {})
        r = subprocess.run([self.exe, self.fake(body), "/usr/local/lib/libsweetgl.dylib"], env=env, capture_output=True, timeout=120)
        tries = int(open(st).read()) if os.path.exists(st) else 0
        return r.returncode, open(log).read() if os.path.exists(log) else "", tries

    COUNT = 'n=$(cat "$STATE" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$STATE"\n'

    def test_pass_first_try(self):
        rc, log, n = self.run_once(self.COUNT + 'echo "sweetgl_selftest: GL_RENDERER=Mali-G78 MC20 (Panfrost) GL_VERSION=x"\necho "sweetgl selftest: clear=PASS tri=PASS"\nexit 0\n')
        self.assertEqual((rc, n), (0, 1))
        self.assertIn("sweetgl selftest: clear=PASS tri=PASS", log)
        self.assertIn("sweetgl_once: done after 1 try", log)

    def test_retry_until_service(self):
        body = self.COUNT + ('if [ $n -lt 3 ]; then echo "sweetgl: no SweetMali service (IOServiceOpen failed)" >&2; '
                             'echo "sweetgl selftest: clear=SKIP tri=SKIP"; exit 1; fi\n'
                             'echo "sweetgl_selftest: GL_RENDERER=x"\necho "sweetgl selftest: clear=PASS tri=FAIL"\nexit 1\n')
        rc, log, n = self.run_once(body)
        self.assertEqual((rc, n), (1, 3))
        self.assertIn("no SweetMali service", log)              # stderr forwarded too
        self.assertIn("clear=PASS tri=FAIL", log)

    def test_gpu_work_never_repeated(self):
        for body in ('echo "sweetgl_selftest: GL_RENDERER=x"\necho "sweetgl selftest: clear=SKIP tri=SKIP"\nexit 1\n',   # renderer seen
                     'echo "sweetgl selftest: clear=FAIL tri=SKIP"\nexit 1\n',
                     'echo "half a line"\nkill -SEGV $$\n',                                                             # crash, no result
                     'exit 0\n'):                                                                                        # no result line
            rc, log, n = self.run_once(self.COUNT + body)
            self.assertEqual(n, 1, body)
            self.assertNotEqual(rc, 0, body)

    def test_max_tries(self):
        rc, log, n = self.run_once(self.COUNT + 'echo "sweetgl selftest: clear=SKIP tri=SKIP"\nexit 1\n', extra_env={"SWEETGL_ONCE_MAX": "4"})
        self.assertEqual(n, 4)
        self.assertEqual(rc, 2)
        self.assertIn("sweetgl_once: gave up after 4 tries", log)

    def test_hung_child_is_killed(self):
        rc, log, n = self.run_once(self.COUNT + 'echo "sweetgl_selftest: GL_RENDERER=x"\nsleep 30\n', extra_env={"SWEETGL_ONCE_TRY_MS": "500"})
        self.assertEqual(n, 1)
        self.assertNotEqual(rc, 0)
        self.assertIn("timed out", log)

    def test_output_bounded(self):
        rc, log, n = self.run_once(self.COUNT + 'yes 0123456789abcdef0123456789abcdef | head -c 4000000\necho "sweetgl selftest: clear=PASS tri=PASS"\nexit 0\n')
        self.assertLess(len(log), 200000)
        self.assertEqual(n, 1)

    def test_long_line_split_not_overflowed(self):
        rc, log, n = self.run_once(self.COUNT + 'head -c 5000 /dev/zero | tr "\\0" "x"; echo\necho "sweetgl selftest: clear=PASS tri=PASS"\nexit 0\n')
        self.assertEqual((rc, n), (0, 1))

    def test_usage(self):
        r = subprocess.run([self.exe], capture_output=True)
        self.assertEqual(r.returncode, 64)


if __name__ == "__main__":
    unittest.main()
