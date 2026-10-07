"""The oracle check (DV_ORACLE): dv-solve-smt2's answers checked by a reference
solver, and the run directory it records. See docs/oracle_check_plan.md.

The checker itself is checked two ways: against z3 on problems where dv-solve
is right (everything must come back ok, and stdout must not change), and
against a fake oracle that disagrees on purpose (the disagreement must be
classified and bundled).
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
EXE = REPO / "build" / "dv-solve-smt2"
FAKE = Path(__file__).with_name("oracle_fake.py")


def _z3():
    for p in (REPO.parent / "tools" / "z3" / "bin" / "z3", shutil.which("z3")):
        if p and Path(p).is_file():
            return str(p)
    return None


Z3 = _z3()
pytestmark = [
    pytest.mark.skipif(not EXE.is_file(), reason="dv-solve-smt2 not built"),
    pytest.mark.skipif(sys.platform == "win32", reason="the oracle is POSIX only"),
]
needs_z3 = pytest.mark.skipif(Z3 is None, reason="z3 not available")

BASIC = """\
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(declare-const y (_ BitVec 8))
(declare-fun b () Bool)
(assert (bvult x y))
(assert (= b (= x #x05)))
(check-sat)
(get-value (x y))
(push 1)
(assert (= x #xff))
(check-sat)
(pop 1)
(assert (= (bvadd x y) #x10))
(check-sat)
(get-value (x y b))
"""


def _run(script, tmp_path, oracle="z3", args=(), env=None, name="run"):
    e = dict(os.environ)
    for k in list(e):
        if k.startswith("DV_ORACLE"):
            del e[k]
    out = tmp_path / name
    if oracle is not None:
        if oracle == "z3":
            e["DV_ORACLE"] = "z3"
            e["DV_ORACLE_BIN"] = Z3 or "z3"
        else:
            e["DV_ORACLE"] = f"cmd:{sys.executable} {FAKE} {oracle}"
        e["DV_ORACLE_OUT"] = str(out)
    e["DV_LOG"] = str(tmp_path / "dv.log")
    e.update(env or {})
    p = subprocess.run([str(EXE), "--interactive", *args], input=script, env=e,
                       capture_output=True, text=True, timeout=120)
    return p, out


def _records(out):
    return [json.loads(l) for l in (out / "queries.jsonl").read_text().splitlines()]


def _run_json(out):
    return json.loads((out / "run.json").read_text())


@needs_z3
def test_agreement_is_recorded(tmp_path):
    p, out = _run(BASIC, tmp_path)
    assert p.returncode == 0, p.stderr
    rj = _run_json(out)
    assert rj["status"] == "complete"
    assert rj["oracle"]["name"] == "z3"
    assert rj["summary"]["queries"] == 3 and rj["summary"]["ok"] == 3
    recs = _records(out)
    assert [r["dvs"]["verdict"] for r in recs] == ["sat", "unsat", "sat"]
    assert [r["orc"]["check"] for r in recs] == ["model", "solve", "model"]
    assert all(r["res"] == "ok" for r in recs)
    assert recs[0]["orc"]["pinned"] == 3          # x, y and the Bool b
    assert not any((out / "fail").iterdir())


@needs_z3
def test_stdout_is_unchanged(tmp_path):
    a, _ = _run(BASIC, tmp_path, oracle=None)
    b, _ = _run(BASIC, tmp_path)
    assert a.stdout == b.stdout


def test_bad_model_is_caught_and_bundled(tmp_path):
    p, out = _run(BASIC, tmp_path, oracle="unsat")
    recs = _records(out)
    bad = [r for r in recs if r["res"] == "bad-model"]
    assert len(bad) == 2                          # both sat answers
    f = out / (bad[0]["file"] + ".smt2")
    text = f.read_text()
    assert "(declare-const x (_ BitVec 8))" in text
    assert "(assert (= b " in text                # the pins, Bool as Bool
    assert "(push" not in text                    # the stack is flattened
    detail = json.loads((out / (bad[0]["file"] + ".json")).read_text())
    assert detail["dvs_model"].startswith("(assert (= x #b")


def test_bad_unsat_is_caught(tmp_path):
    p, out = _run(BASIC, tmp_path, oracle="sat")
    recs = _records(out)
    assert [r["res"] for r in recs] == ["ok", "bad-unsat", "ok"]
    assert recs[1]["file"]
    # The flattened stack at the unsat query includes the pushed assert.
    text = (out / (recs[1]["file"] + ".smt2")).read_text()
    assert "(assert (= x #xff))" in text


def test_pop_drops_the_scope(tmp_path):
    p, out = _run(BASIC, tmp_path, oracle="unsat")
    rec = _records(out)[2]
    text = (out / (rec["file"] + ".smt2")).read_text()
    assert "(assert (= x #xff))" not in text      # popped before query 2


def test_abort_policy(tmp_path):
    p, out = _run(BASIC, tmp_path, oracle="unsat", env={"DV_ORACLE_ON_FAIL": "abort"})
    assert p.returncode == 3
    assert _run_json(out)["status"] == "aborted"


def test_dead_oracle_is_an_oracle_error(tmp_path):
    p, out = _run(BASIC, tmp_path, oracle="die")
    assert p.returncode == 0
    assert {r["res"] for r in _records(out)} == {"oracle-error"}


def test_run_directory_is_never_shared(tmp_path):
    _run(BASIC, tmp_path, oracle="unsat", name="same")
    p, _ = _run(BASIC, tmp_path, oracle="unsat", name="same")
    others = [d for d in tmp_path.iterdir() if d.name.startswith("same-")]
    assert len(others) == 1 and (others[0] / "run.json").exists()


def test_pattern_expansion(tmp_path):
    p, out = _run(BASIC, tmp_path, oracle="unsat", name="r-%p")
    runs = [d for d in tmp_path.iterdir() if d.name.startswith("r-")]
    assert len(runs) == 1 and runs[0].name[2:].isdigit()


def test_sampling_skips(tmp_path):
    p, out = _run(BASIC, tmp_path, oracle="unsat", env={"DV_ORACLE_RATE": "0"})
    assert {r["res"] for r in _records(out)} == {"skipped"}
    assert _run_json(out)["summary"]["skipped"] == 3


VLT_DIVERSITY = """\
(set-logic QF_BV)
(declare-fun x () (_ BitVec 8))
(assert (bvult x #x40))
(check-sat)
(declare-fun a0 () Bool)
(assert (= a0 (= ((_ extract 0 0) x) #b1)))
(declare-fun a1 () Bool)
(assert (= a1 (= ((_ extract 1 1) x) #b0)))
(check-sat-assuming (a0 a1))
(get-value (x))
"""


@needs_z3
def test_verilator_diversity_leaves_late_literals_free(tmp_path):
    """check-sat-assuming re-emits the earlier model; the assumption literals
    declared after it are not part of that model and must not be pinned."""
    p, out = _run(VLT_DIVERSITY, tmp_path, args=["--mode=verilator"])
    recs = _records(out)
    assert recs[1]["sem"] == "vlt-diversity" and recs[1]["dvs"]["engine"] == "reused"
    assert [r["res"] for r in recs] == ["ok", "ok"]
    assert recs[1]["orc"]["pinned"] == 1          # x only


VLT_PARITY = """\
(set-logic QF_BV)
(declare-fun x () (_ BitVec 8))
(assert (= x #x02))
(check-sat)
(assert (= #b1 ((_ extract 0 0) x)))
(check-sat)
(get-value (x))
"""


@needs_z3
def test_ignored_parity_assert_is_not_forwarded(tmp_path):
    """--verilator-hash=ignore keeps the model across a parity assert (here
    one that contradicts it); the oracle must check the problem dv-solve kept."""
    p, out = _run(VLT_PARITY, tmp_path,
                  args=["--mode=verilator", "--verilator-hash=ignore"])
    recs = _records(out)
    assert recs[1]["sem"] == "vlt-parity-skip"
    assert [r["res"] for r in recs] == ["ok", "ok"]


@needs_z3
def test_honoured_parity_assert_is_checked(tmp_path):
    p, out = _run(VLT_PARITY, tmp_path, args=["--mode=verilator"])
    recs = _records(out)
    assert recs[1]["dvs"]["verdict"] == "unsat" and recs[1]["res"] == "ok"


@needs_z3
@pytest.mark.xfail(strict=True, reason=(
    "dv-solve bug found by the oracle: in verilator mode check-sat-assuming "
    "re-emits the previous model even when a blocking clause asserted since "
    "excludes it (Verilator 5.053 bsat), so UniGen2 gets duplicate witnesses"))
def test_verilator_diversity_after_blocking_clause(tmp_path):
    e = dict(os.environ)
    e.update({"DV_ORACLE": "z3", "DV_ORACLE_BIN": Z3, "DV_ORACLE_OUT": str(tmp_path / "run"),
              "DV_LOG": str(tmp_path / "dv.log")})
    p = subprocess.Popen([str(EXE), "--interactive", "--mode=verilator"], env=e, text=True,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    p.stdin.write("(set-logic QF_BV)\n(declare-fun x () (_ BitVec 8))\n"
                  "(assert (bvult x #x40))\n(check-sat)\n(get-value (x))\n")
    p.stdin.flush()
    assert p.stdout.readline().strip() == "sat"
    val = p.stdout.readline().strip().split()[-1].rstrip(")")
    p.stdin.write(f"(assert (not (and true (= x {val}))))\n"
                  f"(declare-fun d0 () Bool)\n(assert (= d0 (not (= x {val}))))\n"
                  "(check-sat-assuming (d0))\n")
    p.stdin.close()
    p.wait(timeout=60)
    recs = _records(tmp_path / "run")
    assert recs[1]["res"] == "ok"


def test_report_tool(tmp_path):
    _run(BASIC, tmp_path, oracle="unsat", name="r1")
    _run(BASIC, tmp_path, oracle="unsat", name="r2", env={"DV_ORACLE_RATE": "0"})
    env = dict(os.environ, PYTHONPATH=str(REPO / "src"))
    p = subprocess.run([sys.executable, "-m", "dv_solve.oracle", "report", str(tmp_path)],
                       env=env, capture_output=True, text=True)
    assert p.returncode == 1                      # P0 present
    js = json.loads((tmp_path / "oracle-report.json").read_text())
    assert js["totals"] == {"bad-model": 2, "ok": 1, "skipped": 3}
    assert js["groups"][0]["res"] == "bad-model" and js["groups"][0]["count"] == 2


@needs_z3
def test_extract_from_transcript(tmp_path):
    p, out = _run(BASIC, tmp_path, env={"DV_ORACLE_KEEP": "all"})
    env = dict(os.environ, PYTHONPATH=str(REPO / "src"))
    q = subprocess.run([sys.executable, "-m", "dv_solve.oracle", "extract", str(out), "1"],
                       env=env, capture_output=True, text=True)
    assert q.returncode == 0, q.stderr
    script = q.stdout
    assert "(assert (= x #xff))" in script
    r = subprocess.run([Z3, "-in"], input=script, capture_output=True, text=True)
    assert r.stdout.split()[0] == "unsat"


def test_keep_summary_writes_counts_only(tmp_path):
    p, out = _run(BASIC, tmp_path, oracle="unsat", env={"DV_ORACLE_KEEP": "summary"})
    assert not (out / "queries.jsonl").exists()
    assert _run_json(out)["summary"]["bad-model"] == 2
    assert len(list((out / "fail").glob("*.smt2"))) == 2
    env = dict(os.environ, PYTHONPATH=str(REPO / "src"))
    subprocess.run([sys.executable, "-m", "dv_solve.oracle", "report", str(tmp_path)],
                   env=env, capture_output=True, text=True)
    js = json.loads((tmp_path / "oracle-report.json").read_text())
    assert js["totals"] == {"bad-model": 2, "ok": 1}
