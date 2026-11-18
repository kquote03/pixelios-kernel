#!/usr/bin/env python3
"""mk_launchd_job.py IN OUT --sha256 HEX [--job sweetgl|sweetaudio]: the replacement /System/Library/xpc/launchd.plist for the G5 self-test run (docs/AUTOMATION.md "G5").

OUT is IN (the launchd job cache extracted read-only from the flashed root image) plus exactly one LaunchDaemons entry, the job that starts
/usr/local/bin/sweetgl_once once at boot (--job sweetaudio: /usr/local/bin/sweetaudio_install, doc 54). Nothing else is added, removed or changed; tools/test_mk_launchd_job.py checks that. The input must
have the given sha256 (so the replacement is made from the cache that is on the phone, not from another root), must be version 7 with a
LaunchDaemons dictionary, and must not already hold the job or its label. Exit 0 = written, 2 = refused (nothing written)."""
import hashlib, os, plistlib, sys

JOBS = {
    "sweetgl": ("org.pixelios.sweetgl-selftest",
                ["/usr/local/bin/sweetgl_once", "/usr/local/bin/sweetgl_selftest", "/usr/local/lib/libsweetgl.dylib"]),
    "sweetaudio": ("org.pixelios.sweetaudio", ["/usr/local/bin/sweetaudio_install"]),   # mode from /usr/local/pixos/sweetaudio.mode
}
KEY = LABEL = JOB = None


def select(name):
    global KEY, LABEL, JOB
    LABEL, args = JOBS[name]
    KEY = "/System/Library/LaunchDaemons/%s.plist" % LABEL
    JOB = {"Label": LABEL, "RunAtLoad": True, "ProgramArguments": list(args)}


select("sweetgl")
VERSION = 7     # the cache version of the iOS 16 root this was written against (observed in the extracted cache)


def refuse(msg):
    print("mk_launchd_job: refused: " + msg, file=sys.stderr)
    return 2


def main(argv):
    if len(argv) == 7 and argv[5] == "--job" and argv[6] in JOBS:
        select(argv[6])
        argv = argv[:5]
    if len(argv) != 5 or argv[3] != "--sha256" or len(argv[4]) != 64:
        return refuse("usage: mk_launchd_job.py IN OUT --sha256 <64 hex digits of IN> [--job %s]" % "|".join(sorted(JOBS)))
    src, dst, want = argv[1], argv[2], argv[4].lower()
    raw = open(src, "rb").read()
    got = hashlib.sha256(raw).hexdigest()
    if got != want:
        return refuse("input sha256 is %s, not %s" % (got, want))
    try:
        pl = plistlib.loads(raw)
    except Exception as e:                                              # any parse failure
        return refuse("input is not a property list (%s)" % type(e).__name__)
    if not isinstance(pl, dict) or not isinstance(pl.get("LaunchDaemons"), dict) or pl.get("VersionNumber") != VERSION:
        return refuse("input is not a version %d launchd cache with a LaunchDaemons dictionary" % VERSION)
    jobs = pl["LaunchDaemons"]
    if KEY in jobs or any(isinstance(j, dict) and j.get("Label") == LABEL for j in jobs.values()):
        return refuse("the job or its label is already in the cache")
    n = len(jobs)
    jobs[KEY] = dict(JOB)
    out = plistlib.dumps(pl, fmt=plistlib.FMT_BINARY, sort_keys=True)
    back = plistlib.loads(out)
    if back["LaunchDaemons"].pop(KEY) != JOB or back != plistlib.loads(raw):
        return refuse("the written cache does not read back as input + one job")
    tmp = dst + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(out)
    os.replace(tmp, dst)
    print("mk_launchd_job: jobs %d -> %d, %d bytes, sha256 %s" % (n, n + 1, len(out), hashlib.sha256(out).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
