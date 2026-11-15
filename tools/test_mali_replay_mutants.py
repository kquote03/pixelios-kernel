"""Mutation test of the Mali job-replay parser in xnu patch 0155 (bsd/dev/sweet_mali_replay.h, plan oneshot-2 G2, docs/research/52 s.6).

Every bounds/VA/limit rule of smr_container_parse / smr_job_parse / mali_va_reserve / smr_rt_check is removed or weakened in a copy of the
header (every occurrence of the pattern); shim/mali_replay_host_test.c built against the copy must FAIL. A mutant that still passes is a survivor
and fails this test; a pattern that matches nothing fails too (the header changed under the mutant list).
Run: python3 -B tools/test_mali_replay_mutants.py  (also picked up by `unittest discover -s tools -p 'test_*.py'`)."""
import concurrent.futures
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P0104 = os.path.join(ROOT, "xnu", "patches", "0104-sweet-mali-driver-core.patch")
P0155 = os.path.join(ROOT, "xnu", "patches", "0155-sweet-mali-replay.patch")
TEST = os.path.join(ROOT, "shim", "mali_replay_host_test.c")


def put(path, text):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def new_file(patch, path):
    """the text of a new-file hunk (the shim/build.sh awk rule)"""
    out, on = [], False
    with open(patch, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    for line in lines:
        if line == "+++ b/" + path:
            on = True
            continue
        if on and (line.startswith("--- ") or line.startswith("diff ")):
            break
        if on and line.startswith("@@"):
            continue
        if on and line.startswith("+"):
            out.append(line[1:])
    return "\n".join(out) + "\n"


# (name, pattern, replacement): each must be killed by the host test
MUTANTS = [
    ("container length floor", "if (len < SMR_C_HDR || len > SMR_C_MAX_BYTES) {\n\t\treturn 1;", "if (len > SMR_C_MAX_BYTES) {\n\t\treturn 1;"),
    ("container length cap", "if (len < SMR_C_HDR || len > SMR_C_MAX_BYTES) {\n\t\treturn 1;", "if (len < SMR_C_HDR) {\n\t\treturn 1;"),
    ("container magic", "if (buf[i] != magic[i]) {\n\t\t\treturn 2;", "if (0) {\n\t\t\treturn 2;"),
    ("container count cap", "n == 0 || n > SMR_C_MAX_ENTRIES ||", "n == 0 ||"),
    ("container table fits", "|| SMR_C_HDR + SMR_C_ENTRY * n > len) {\n\t\treturn 3;", ") {\n\t\treturn 3;"),
    ("container tag", "if (r[0] != 'G' || r[1] != 'P' || r[2] != 'U' || r[3] != 'J') {", "if (r[0] != 'G') {"),
    ("container entry offset aligned", "(e[i].offset & 7u) != 0 ||", ""),
    ("container entry length in buffer", "||\n\t\t    e[i].length > len - e[i].offset) {", ") {"),
    ("container entry after the table", "e[i].offset < SMR_C_HDR + SMR_C_ENTRY * n ||", ""),
    ("container crc", "if (smr_crc32(0, buf + e[i].offset, e[i].length) != e[i].crc) {", "if (0) {"),
    ("job file_size", "|| smr_le64(buf + 32) != len) {", ") {"),
    ("job magic", "if (buf[i] != magic[i]) {\n\t\t\treturn SMR_E_MAGIC;", "if (0) {\n\t\t\treturn SMR_E_MAGIC;"),
    ("job chain cap", "j->n_chains > SMR_MAX_CHAINS ||", ""),
    ("job bo cap", "j->n_bos > SMR_MAX_BOS", "0"),
    ("job rt flag", "flags != 1u ||", ""),
    ("job gpu id", "if (j->gpu_id != want_gpu_id) {", "if (0) {"),
    ("table inside file", "return off >= SMR_J_HDR && off <= len && size <= len - off;", "return off >= SMR_J_HDR;"),
    ("table after header", "return off >= SMR_J_HDR && off <= len && size <= len - off;", "return off <= len && size <= len - off;"),
    ("name charset", "if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {", "if (0) {"),
    ("bo size zero", "if (b.size == 0 || b.size > SMR_MAX_BO_BYTES ||", "if (b.size > SMR_MAX_BO_BYTES ||"),
    ("bo size cap", "b.size > SMR_MAX_BO_BYTES ||", ""),
    ("bo size page", "(b.size & (SMR_BO_PAGE - 1)) ||", ""),
    ("bo va page", "(b.va & (SMR_BO_PAGE - 1)) ||", ""),
    ("bo va start", "b.va < MALI_VA_START ||", ""),
    ("bo va end", "b.size > MALI_VA_END - b.va ||", ""),
    ("bo data within size", "b.data_len > b.size ||", ""),
    ("bo flags", "(b.flags & ~SMR_BO_FLAGS) ||", ""),
    ("bo label nul", "r[63] != 0 ||", ""),
    ("bo label printable (F2)", "if (r[40 + k] < 0x20u || r[40 + k] > 0x7eu) {", "if (0) {"),
    ("bo data after tables", "b.data_off < data_off ||", ""),
    ("bo data in file", "|| b.data_len > len - b.data_off) {\n\t\t\treturn SMR_E_BO;", ") {\n\t\t\treturn SMR_E_BO;"),
    ("bo overlap", "if (b.va < o.va + o.size && o.va < b.va + b.size) {", "if (0) {"),
    ("chain align", "(j->jc[i] & (SMR_JOB_ALIGN - 1)) ||", ""),
    ("chain req bits", "(j->req[i] & ~SMR_REQ_FS) ||", ""),
    ("chain in a BO", "|| smr_find_bo(j, j->jc[i], SMR_JOB_MIN) < 0) {", ") {"),
    ("chain needs fragment", "if (!fs) {\n\t\treturn SMR_E_CHAIN;", "if (0) {\n\t\treturn SMR_E_CHAIN;"),
    ("find_bo size fits", "size <= b.size - (va - b.va)", "1"),
    ("rt format", "j->rt.format != 1u ||", ""),
    ("rt width cap", "j->rt.width > SMR_MAX_RT_DIM ||", ""),
    ("rt stride", "(uint64_t)j->rt.stride < (uint64_t)j->rt.width * 4u ||", ""),
    ("rt crc_valid", "|| j->rt.crc_valid > 1u) {", ") {"),
    ("rt in a BO", "if (bi < 0) {\n\t\t\treturn SMR_E_RT;", "if (bi < -1) {\n\t\t\treturn SMR_E_RT;"),
    ("rt pixel inside", "if (j->rt.px_x[i] >= j->rt.width || j->rt.px_y[i] >= j->rt.height) {", "if (0) {"),
    ("rt check pixels", "if (smr_le32(rt + (size_t)j->rt.px_y[i] * j->rt.stride + (size_t)j->rt.px_x[i] * 4u) != j->rt.px_rgba[i]) {", "if (0) {"),
    ("rt check crc", "if (j->rt.crc_valid && c != j->rt.expect_crc) {", "if (0) {"),
    ("reserve taken", "if (mali_va_test(v, i + k)) {\n\t\t\treturn -2;", "if (0) {\n\t\t\treturn -2;"),
    ("reserve window end", "(uint64_t)npages * MALI_PG > MALI_VA_END - va) {", "0) {"),
    ("reserve alignment", "(npages & 3u) || (va & (SMR_BO_PAGE - 1)) ||", ""),
    ("reserve below window", "(va & (SMR_BO_PAGE - 1)) || va < MALI_VA_START ||", "(va & (SMR_BO_PAGE - 1)) ||"),
]


def run_mutant(hdr_pt, hdr, m):
    name, pat, rep = m
    if pat not in hdr:
        return name, "pattern-missing"
    with tempfile.TemporaryDirectory() as d:
        put(os.path.join(d, "sweet_mali_pt.h"), hdr_pt)
        put(os.path.join(d, "sweet_mali_replay.h"), hdr.replace(pat, rep))
        exe = os.path.join(d, "t")
        c = subprocess.run(["gcc", "-O1", "-g", "-w", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + d, "-o", exe, TEST],
                           capture_output=True)
        if c.returncode != 0:
            return name, "killed(compile)"
        r = subprocess.run([exe], capture_output=True, timeout=120)
        return name, "survived" if r.returncode == 0 else "killed"


class ReplayMutants(unittest.TestCase):
    def test_all_killed(self):
        hdr_pt = new_file(P0104, "bsd/dev/sweet_mali_pt.h")
        hdr = new_file(P0155, "bsd/dev/sweet_mali_replay.h")
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
            res = list(ex.map(lambda m: run_mutant(hdr_pt, hdr, m), MUTANTS))
        bad = [(n, s) for n, s in res if s in ("survived", "pattern-missing")]
        for n, s in res:
            print("%-34s %s" % (n, s))
        self.assertEqual(bad, [], "survivors / stale patterns")

    def test_unmutated_passes(self):
        with tempfile.TemporaryDirectory() as d:
            put(os.path.join(d, "sweet_mali_pt.h"), new_file(P0104, "bsd/dev/sweet_mali_pt.h"))
            put(os.path.join(d, "sweet_mali_replay.h"), new_file(P0155, "bsd/dev/sweet_mali_replay.h"))
            exe = os.path.join(d, "t")
            subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + d, "-o", exe, TEST], check=True)
            self.assertEqual(subprocess.run([exe], capture_output=True).returncode, 0)


if __name__ == "__main__":
    unittest.main()
