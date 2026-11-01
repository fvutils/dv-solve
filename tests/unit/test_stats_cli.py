"""Stats files, merge, bundle and audit (docs/solve_stats_capture_plan.md, S2).

A process run with DV_STATS=<path> writes its stats file at exit; files merge
exactly; a bundle is salted, coarsened to powers of two and k-thresholded; the
audit passes every file the library writes and fails one with a name, an
unknown key or free text in it.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

from dv_solve import stats as S

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "src" / "c"

# Solves through the Python API: `n` of x < y, and one of x < y && x > y.
_RUN = r"""
import sys
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx
from dv_solve.problem import BIN_LT, BIN_GT
n = int(sys.argv[1])
def problem(unsat):
    b = SolveProblemBuilder()
    for i in (0, 1):
        b.add_var(i, 8, False, 0, 255)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_var(1)))
    if unsat:
        b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(0), b.expr_var(1)))
    return b
for unsat, k in ((False, n), (True, 1)):
    b = problem(unsat)
    buf, _ = b.finalize()
    with SolveCtx(buf) as ctx:
        for s in range(k):
            ctx.reset()
            ctx.solve(seed=s + 1)
"""


def _env(**extra):
    env = dict(os.environ, **extra)
    env.pop("LD_LIBRARY_PATH", None)
    env["PYTHONPATH"] = os.pathsep.join([str(REPO / "src"), env.get("PYTHONPATH", "")])
    return env


def _run(tmp_path, name, n):
    out = tmp_path / f"{name}.json"
    env = _env(DV_STATS=str(out))
    r = subprocess.run([sys.executable, "-c", _RUN, str(n)], env=env, cwd=REPO,
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    return S.load(str(out))


@pytest.fixture
def files(tmp_path):
    return _run(tmp_path, "a", 5), _run(tmp_path, "b", 7)


def _c_table(text, name):
    m = re.search(r"static const char \*const " + name + r"\[[^\]]*\] = \{(.*?)\};", text, re.S)
    return [x for x in re.findall(r'"([^"]*)"|NULL', m.group(1))]


def test_vocabulary_matches_the_library():
    st = (SRC / "dvs_stats.c").read_text()
    cl = (SRC / "dvs_class.c").read_text()
    assert _c_table(st, "k_path")[:len(S.PATHS)] == S.PATHS
    assert _c_table(st, "k_engine") == S.ENGINES
    assert _c_table(st, "k_answered") == S.ANSWERED
    assert _c_table(st, "k_result") == S.RESULTS
    assert _c_table(st, "k_via") == S.VIA
    assert _c_table(st, "k_reason") == S.REASONS
    assert [b for b in _c_table(st, "k_bail") if b] == S.BAILS[:-1]
    assert _c_table(cl, "k_motif") == S.MOTIFS


def test_file_at_exit(files):
    a, _ = files
    s = a["snapshot"]
    assert a["schema"] == S.FILE_SCHEMA and s["solves"] == 6
    assert s["results"]["sat"] == 5 and s["results"]["unsat"] == 1
    assert len(s["classes"]) == 2
    assert all(len(c["hll"]) == 64 for c in s["classes"])
    sat = next(c for c in s["classes"] if c["solves"] == 5)
    v = sat["values"]
    assert v["models"] == 5 and v["mismatched"] == 0 and 1 <= v["solutions"] <= 5
    assert [x["width"] for x in v["vars"]] == [8, 8] and len(v["hll"]) == 64
    unsat = next(c for c in s["classes"] if c["solves"] == 1)
    assert "values" not in unsat                     # no model, no record
    assert S.audit(a) == []


def test_merge_is_exact(files):
    a, b = files
    m = S.merge([a, b])
    s = m["snapshot"]
    assert s["solves"] == 6 + 8
    by = {c["solves"]: c for c in s["classes"]}
    assert set(by) == {12, 2}
    assert by[12]["instances"] == 1                  # one instance, seen in both runs
    assert by[12]["paths"]["api"]["sat"] == 12
    assert by[12]["values"]["models"] == 12
    assert [x["width"] for x in by[12]["values"]["vars"]] == [8, 8]
    assert all(x["varies"] and not x["stuck"] for x in by[12]["values"]["vars"])
    assert s["motifs"]["var-cmp-var"]["count"] == 1 + 2
    # signals 3, 5 and 6 add up over the runs
    assert by[12]["root"]["count"] == 2 and by[12]["root"]["bits"] == 2 * 16
    assert by[12]["binding"]["solves"] == 12 and by[12]["binding"]["narrowed"] == 12
    assert by[12]["previous"]["same"] == 4 + 6 and by[12]["time_ns"]["compile"] > 0
    assert "root: 2 vars, 0 fixed, domain 2^16 -> 2^16" in S.report(m)
    # merge is associative and order-free
    assert S.merge([b, a])["snapshot"]["solves"] == s["solves"]
    assert S.merge([S.merge([a]), b])["snapshot"]["classes"] == s["classes"]
    assert S.audit(m) == []


def test_bundle(files):
    m = S.merge(files)
    b = S.bundle(m, salt=b"x" * 32, k=3)
    s = b["snapshot"]
    assert b["schema"] == S.BUNDLE_SCHEMA and b["salted"] and b["level"] == "L0"
    ids = [c["class"] for c in s["classes"]]
    # the 2-solve unsat class is under k: folded
    assert ids.count(S.OTHER_CLASS) == 1 and len(ids) == 2
    kept = next(c for c in s["classes"] if c["class"] != S.OTHER_CLASS)
    assert len(kept["class"]) == 32
    assert kept["class"] not in {c["class"] for c in m["snapshot"]["classes"]}
    assert kept["solves"] == 8                       # 12, down to a power of two
    assert "hll" not in kept and "hll" not in kept["values"]
    assert kept["values"]["models"] == 8
    assert S.audit(b) == []
    # the same salt gives the same ids; another salt does not
    assert S.bundle(m, salt=b"x" * 32)["snapshot"]["classes"][0]["class"] == ids[0]
    assert S.bundle(m, salt=b"y" * 32)["snapshot"]["classes"][0]["class"] != ids[0]
    # no salt: the class ids are the file's
    nb = S.bundle(m, salt=None)
    assert nb["snapshot"]["classes"][0]["class"] == m["snapshot"]["classes"][0]["class"]


def test_audit_fails_on_injected_content(files):
    b = S.bundle(S.merge(files), salt=b"s" * 32)
    bad = json.loads(json.dumps(b))
    bad["snapshot"]["classes"][0]["note"] = "x"                      # unknown key
    assert any("not in the schema" in p for p in S.audit(bad))
    bad = json.loads(json.dumps(b))
    bad["snapshot"]["classes"][0]["class"] = "pkt_item"               # a name as id
    assert any("not a hash" in p for p in S.audit(bad))
    bad = json.loads(json.dumps(b))
    bad["snapshot"]["via"]["cdcl-search"] = "fast"                    # free text
    assert any("not a schema value" in p for p in S.audit(bad))
    bad = json.loads(json.dumps(b))
    bad["snapshot"]["solves"] = 13                                    # not coarsened
    assert any("not a power of two" in p for p in S.audit(bad))
    # words: absent from a real bundle, found when injected, in clear or hashed
    assert S.audit(b, ["my_pkt_item", "dma_cfg"]) == []
    text = json.dumps(b).replace('"L0"', '"L0", "x": "my_pkt_item"')
    assert any("my_pkt_item" in p for p in S.audit(b, ["my_pkt_item"], text))
    import hashlib
    text = json.dumps(b) + hashlib.sha256(b"dma_cfg").hexdigest()
    assert any("sha256" in p for p in S.audit(b, ["dma_cfg"], text))


def test_cli(files, tmp_path):
    a, b = files
    pa, pb = tmp_path / "a.json", tmp_path / "b.json"
    out, salt = tmp_path / "bundle.json", tmp_path / "salt"
    pa.write_text(json.dumps(a))
    pb.write_text(json.dumps(b))
    env = _env()
    run = lambda *args: subprocess.run([sys.executable, "-m", "dv_solve.stats", *args],  # noqa: E731
                                       env=env, cwd=REPO, capture_output=True, text=True,
                                       timeout=60)
    r = run("report", str(pa), str(pb))
    assert r.returncode == 0 and "14 solves" in r.stdout, r.stdout + r.stderr
    r = run("bundle", "-o", str(out), "--salt-file", str(salt), str(pa), str(pb))
    assert r.returncode == 0, r.stderr
    first = json.loads(out.read_text())["snapshot"]["classes"][0]["class"]
    r = run("bundle", "-o", str(out), "--salt-file", str(salt), str(pa), str(pb))
    assert json.loads(out.read_text())["snapshot"]["classes"][0]["class"] == first
    r = run("explain", str(out))
    assert r.returncode == 0 and "solver time" in r.stdout
    r = run("audit", str(out), "--words", "my_pkt_item,addr_cfg")
    assert r.returncode == 0 and "ok" in r.stdout, r.stdout


def test_merge_values():
    var = {"width": 8, "free": 1, "used": 0, "varies": 0, "max_bias": 0, "stuck": 0}
    a = {"models": 10, "mismatched": 0, "solutions": 1, "hll": [0] * 64, "vars": [dict(var)]}
    b = {**a, "models": 8, "vars": [dict(var, used=3, varies=2, max_bias=100)]}
    m = S._merge_values(a, b)
    assert m["models"] == 18 and m["vars"][0]["varies"] == 2 and m["vars"][0]["stuck"] == 0
    # never varied in 18 models of a free variable: stuck
    assert S._merge_values(a, dict(a))["vars"][0]["stuck"] == 1
    # layouts that differ: the larger is kept, the other is counted as mismatched
    c = {**a, "models": 3, "vars": [dict(var, width=16)]}
    m = S._merge_values(a, c)
    assert m["vars"][0]["width"] == 8 and m["models"] == 10 and m["mismatched"] == 3


_TIMED = r"""
import time
from dv_solve import stats
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx
from dv_solve.problem import BIN_LT
assert stats.enabled()
with stats.timed("build"):          # a front end translating its constraints
    time.sleep(0.02)
    b = SolveProblemBuilder()
    for i in (0, 1):
        b.add_var(i, 8, False, 0, 255)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_var(1)))
    buf, _ = b.finalize()
with SolveCtx(buf) as ctx:
    ctx.solve(seed=1)
"""


def test_front_end_time(tmp_path):
    """A front end driving the API directly (pyvsc) reports its own time."""
    out = tmp_path / "t.json"
    r = subprocess.run([sys.executable, "-c", _TIMED], env=_env(DV_STATS=str(out)),
                       cwd=REPO, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    (c,) = S.load(str(out))["snapshot"]["classes"]
    assert c["paths"]["api"]["sat"] == 1
    assert c["time_ns"]["build"] >= 20_000_000 and c["time_ns"]["compile"] > 0
    assert c.get("root") and c.get("binding") and c.get("values")
