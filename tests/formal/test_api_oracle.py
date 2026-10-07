"""The C API's oracle check (dvs_oracle_create / dvs_solver_set_oracle).

Two questions per answer: does dv-solve's sat/unsat verdict agree with z3, and
do the values dv-solve produced satisfy the constraints as z3 judges them. The
checker is checked against z3 where dv-solve is right (every query ok), and
against a fake reference solver that disagrees on purpose (classified,
bundled, raised). See docs/api_oracle_plan.md.
"""
from __future__ import annotations

import json
import random
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT, CompileUnsatError
from dv_solve.problem import (
    BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV, BIN_MOD, BIN_BAND, BIN_BOR, BIN_BXOR,
    BIN_LSHIFT, BIN_RSHIFT, BIN_ASHR, BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE, BIN_GT,
    BIN_GTE, BIN_AND, BIN_OR, UN_NEG, UN_NOT, UN_INVERT,
)
from dv_solve import oracle as dvo

REPO = Path(__file__).resolve().parents[2]
FAKE = Path(__file__).with_name("oracle_fake.py")


def _z3():
    for p in (REPO.parent / "tools" / "z3" / "bin" / "z3", shutil.which("z3")):
        if p and Path(p).is_file():
            return str(p)
    return None


Z3 = _z3()
pytestmark = pytest.mark.skipif(sys.platform == "win32", reason="the oracle is POSIX only")
needs_z3 = pytest.mark.skipif(Z3 is None, reason="z3 not available")


def _z3_oracle(tmp_path, name="run", **kw):
    return dvo.Oracle(bin=Z3, out_dir=str(tmp_path / name), **kw)


def _fake_oracle(tmp_path, verdict, name="run", **kw):
    return dvo.Oracle(command=f"{sys.executable} {FAKE} {verdict}",
                      out_dir=str(tmp_path / name), **kw)


def _records(run_dir):
    p = Path(run_dir) / "queries.jsonl"
    return [json.loads(l) for l in p.read_text().splitlines()] if p.exists() else []


def _run_json(run_dir):
    return json.loads((Path(run_dir) / "run.json").read_text())


def _simple(b=None):
    """x, y: 8-bit unsigned; x < y; x + y == 20."""
    b = b or SolveProblemBuilder()
    b.add_var(0, 8, False, 0, 255)
    b.add_var(1, 8, False, 0, 255)
    x, y = b.expr_var(0), b.expr_var(1)
    b.add_constraint(b.expr_binary(BIN_LT, x, y))
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_binary(BIN_ADD, x, y),
                                   b.expr_const(20)))
    return b


# --------------------------------------------------------------------- #
# Against z3: dv-solve right, every query ok                             #
# --------------------------------------------------------------------- #

@needs_z3
def test_sat_and_unsat_ok(tmp_path):
    o = _z3_oracle(tmp_path, tag="basic")
    buf, _ = _simple().finalize()
    ctx = SolveCtx(buf, oracle=o, label="x y")
    for seed in range(1, 6):
        ctx.reset()
        assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
        assert o.last_result == "ok"

    b = SolveProblemBuilder()
    b.add_var(0, 4, False, 0, 15)
    x = b.expr_var(0)
    b.add_constraint(b.expr_binary(BIN_GT, x, b.expr_const(10)))
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_binary(BIN_MUL, x, x),
                                   b.expr_const(50)))     # SV: 32-bit context
    buf2, _ = b.finalize()
    try:
        c2 = SolveCtx(buf2, oracle=o)
        assert c2.solve(seed=1) == SOLVE_UNSAT
    except CompileUnsatError:
        pass
    assert o.last_result == "ok"

    run = o.run_dir
    st = o.stats()
    o.close()
    recs = _records(run)
    assert [r["res"] for r in recs] == ["ok"] * 6
    assert recs[0]["cmd"] == "api-solve" and recs[0]["orc"]["check"] == "model"
    assert recs[0]["orc"]["pinned"] == 2 and recs[0]["label"] == "x y"
    assert recs[0]["seed"] == 1 and recs[0]["sem"] == "sv"
    assert recs[-1]["dvs"]["verdict"] == "unsat" and recs[-1]["orc"]["check"] == "solve"
    assert st["queries"] == 6 and st["ok"] == 6
    rj = _run_json(run)
    assert rj["status"] == "complete" and rj["mode"] == "api" and rj["tag"] == "basic"
    assert rj["summary"]["sessions"] == 2


@needs_z3
def test_default_oracle_and_pins(tmp_path):
    """set_default_oracle attaches to every new context; pins, exclusions,
    checkpoints and added constraints are mirrored."""
    o = _z3_oracle(tmp_path)
    prev = dvo.set_default_oracle(o)
    try:
        buf, _ = _simple().finalize()
        ctx = SolveCtx(buf)
        cp = ctx.checkpoint()
        assert ctx.pin(0, 3)
        assert ctx.solve(seed=2) == SOLVE_OK
        assert ctx.get_value(0) == 3 and ctx.get_value(1) == 17
        ctx.restore(cp)
        ctx.reset()
        assert ctx.solve(seed=3) == SOLVE_OK
        # An added constraint, then an unsat one.
        b = SolveProblemBuilder()
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_const(4)))
        aux, _ = b.finalize()
        ctx.reset()
        assert ctx.add_constraint(aux) == 0
        assert ctx.solve(seed=4) == SOLVE_OK and ctx.get_value(0) == 4
        ctx.reset()
        assert not ctx.pin(1, 3) or ctx.solve(seed=5) == SOLVE_UNSAT
    finally:
        dvo.set_default_oracle(prev)
    run = o.run_dir
    o.close()
    assert {r["res"] for r in _records(run)} == {"ok"}


def _rand_expr(b, rng, vars_, depth):
    if depth == 0 or rng.random() < 0.3:
        if rng.random() < 0.6:
            return b.expr_var(rng.choice(vars_))
        if rng.random() < 0.5:
            return b.expr_const(rng.randint(-20, 300))
        w = rng.choice([1, 3, 8, 16, 40])
        return b.expr_const(rng.getrandbits(w), is_signed=rng.random() < 0.5, width=w)
    k = rng.random()
    if k < 0.55:
        op = rng.choice([BIN_ADD, BIN_SUB, BIN_MUL, BIN_BAND, BIN_BOR, BIN_BXOR,
                         BIN_LSHIFT, BIN_RSHIFT, BIN_ASHR, BIN_EQ, BIN_LT, BIN_GTE,
                         BIN_AND, BIN_OR, BIN_NEQ])
        return b.expr_binary(op, _rand_expr(b, rng, vars_, depth - 1),
                             _rand_expr(b, rng, vars_, depth - 1))
    if k < 0.7:
        return b.expr_unary(rng.choice([UN_NEG, UN_NOT, UN_INVERT]),
                            _rand_expr(b, rng, vars_, depth - 1))
    if k < 0.8:
        return b.expr_ite(_rand_expr(b, rng, vars_, depth - 1),
                          _rand_expr(b, rng, vars_, depth - 1),
                          _rand_expr(b, rng, vars_, depth - 1))
    if k < 0.9:
        v = rng.choice(vars_)
        hi = rng.randint(0, 9)
        return b.expr_extract(b.expr_var(v), hi, rng.randint(0, hi))
    return b.expr_in_range(_rand_expr(b, rng, vars_, depth - 1),
                           b.expr_const(rng.randint(-5, 50)),
                           b.expr_const(rng.randint(50, 400)))


@needs_z3
def test_random_problems_agree(tmp_path):
    """Random mixed-width, mixed-signedness problems: z3 must accept every
    dv-solve model and confirm every UNSAT. A failure here is a bug in
    dv-solve or in the oracle's SV reading -- either way worth a look at the
    bundle."""
    o = _z3_oracle(tmp_path)
    rng = random.Random(20261007)
    n_ok = 0
    for i in range(60):
        b = SolveProblemBuilder()
        nv = rng.randint(1, 4)
        for v in range(nv):
            w = rng.choice([1, 4, 8, 12, 16, 32])
            s = rng.random() < 0.4
            if s:
                b.add_var(v, w, True, -(1 << (w - 1)), (1 << (w - 1)) - 1)
            else:
                b.add_var(v, w, False, 0, (1 << w) - 1)
        for _ in range(rng.randint(1, 3)):
            b.add_constraint(_rand_expr(b, rng, list(range(nv)), 3))
        buf, _ = b.finalize()
        try:
            ctx = SolveCtx(buf, oracle=o, label=f"p{i}")
        except Exception:
            continue
        for seed in (1, 2):
            ctx.reset()
            ctx.solve(seed=seed, fair_pick=True, time_limit_ms=2000)
            n_ok += 1
        ctx.destroy()
    run = o.run_dir
    o.close()
    recs = _records(run)
    bad = [r for r in recs if r["res"] not in ("ok", "skipped")]
    assert not bad, json.dumps(bad[:5], indent=1)
    assert len(recs) >= 60


@needs_z3
def test_aggregates_and_alldiff(tmp_path):
    o = _z3_oracle(tmp_path)
    b = SolveProblemBuilder()
    for v in range(4):
        b.add_var(v, 8, False, 0, 9)
    b.add_var(4, 8, False, 0, 255)       # the sum
    b.add_var(5, 8, False, 0, 255)       # countones(v0)
    b.add_var(6, 8, False, 0, 3)         # index
    b.add_var(7, 8, False, 0, 255)       # a[index]
    b.add_all_different([0, 1, 2, 3])
    b.add_constraint(b.expr_sum(b.expr_var(4), [b.expr_var(v) for v in range(4)]))
    b.add_constraint(b.expr_countones(b.expr_var(5), b.expr_var(0)))
    b.add_constraint(b.expr_array_select(0, 4, b.expr_var(7), b.expr_var(6)))
    b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(4), b.expr_const(20)))
    buf, _ = b.finalize()
    ctx = SolveCtx(buf, oracle=o)
    for seed in range(1, 6):
        ctx.reset()
        assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
    run = o.run_dir
    o.close()
    recs = _records(run)
    assert [r["res"] for r in recs] == ["ok"] * 5
    assert recs[0]["sem"] == "sv+agg"


@needs_z3
def test_written_script_catches_a_wrong_value(tmp_path):
    """The translation itself: z3 accepts dv-solve's value and rejects a wrong
    one. 16-bit a, b; 32-bit c == a + b must keep the carry (SV context)."""
    b = SolveProblemBuilder()
    b.add_var(0, 16, False, 0, 65535)
    b.add_var(1, 16, False, 0, 65535)
    b.add_var(2, 32, False, 0, (1 << 32) - 1)
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(2),
                                   b.expr_binary(BIN_ADD, b.expr_var(0), b.expr_var(1))))
    b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(0), b.expr_const(60000)))
    b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(1), b.expr_const(60000)))
    buf, _ = b.finalize()
    text = dvo.problem_smt2(buf).replace("(check-sat)\n", "")
    ctx = SolveCtx(buf, oracle=False)
    assert ctx.solve(seed=1) == SOLVE_OK
    a, bb, c = (ctx.get_value(i) for i in range(3))
    assert c == a + bb > 65535

    def z3(extra):
        p = subprocess.run([Z3, "-in", "-smt2"], input=text + extra + "(check-sat)\n",
                           capture_output=True, text=True)
        return p.stdout.strip()
    pin = f"(assert (= v0 (_ bv{a} 16)))(assert (= v1 (_ bv{bb} 16)))"
    assert z3(pin + f"(assert (= v2 (_ bv{c} 32)))") == "sat"
    assert z3(pin + f"(assert (= v2 (_ bv{c & 0xffff} 32)))") == "unsat"


# --------------------------------------------------------------------- #
# Against a fake: disagreements classified, bundled, raised              #
# --------------------------------------------------------------------- #

def test_bad_model_is_bundled(tmp_path):
    o = _fake_oracle(tmp_path, "unsat")
    buf, _ = _simple().finalize()
    ctx = SolveCtx(buf, oracle=o, label="x y")
    assert ctx.solve(seed=1) == SOLVE_OK
    assert o.last_result == "bad-model"
    run = o.run_dir
    o.close()
    rec = _records(run)[0]
    assert rec["res"] == "bad-model" and rec["file"]
    script = (Path(run) / (rec["file"] + ".smt2")).read_text()
    assert "(declare-const v0 (_ BitVec 8))" in script
    assert "(assert (= v0 (_ bv" in script and script.rstrip().endswith("(check-sat)")
    detail = json.loads((Path(run) / (rec["file"] + ".json")).read_text())
    assert detail["dvs_model"].startswith("(assert (= v0 ")


def test_bad_unsat_raises(tmp_path):
    o = _fake_oracle(tmp_path, "sat", raise_on_fail=True)
    b = SolveProblemBuilder()
    b.add_var(0, 4, False, 0, 15)
    b.add_var(1, 4, False, 0, 15)
    x, y = b.expr_var(0), b.expr_var(1)
    b.add_constraint(b.expr_binary(BIN_LT, x, y))
    b.add_constraint(b.expr_binary(BIN_LT, y, x))
    buf, _ = b.finalize()
    with pytest.raises((dvo.OracleMismatch, CompileUnsatError)) as ei:
        ctx = SolveCtx(buf, oracle=o)
        ctx.solve(seed=1)
    assert ei.type is dvo.OracleMismatch
    assert o.last_result == "bad-unsat"
    o.close()


def test_report_tool_reads_api_runs(tmp_path):
    o = _fake_oracle(tmp_path, "unsat", name="runs/a")
    buf, _ = _simple().finalize()
    ctx = SolveCtx(buf, oracle=o)
    ctx.solve(seed=1)
    ctx.destroy()
    o.close()
    rc = dvo.report([str(tmp_path / "runs")], tmp_path / "rep")
    assert rc == 1
    js = json.loads((tmp_path / "rep" / "oracle-report.json").read_text())
    assert js["totals"] == {"bad-model": 1}
    assert js["groups"][0]["cmd"] == "api-solve" and js["groups"][0]["engine"] == "api"


def test_close_detaches_live_contexts(tmp_path):
    o = _fake_oracle(tmp_path, "sat")
    buf, _ = _simple().finalize()
    ctx = SolveCtx(buf, oracle=o)
    o.close()                       # ctx still alive: must not dangle
    ctx.reset()
    assert ctx.solve(seed=1) == SOLVE_OK
    ctx.destroy()


def test_bad_command_raises(tmp_path):
    with pytest.raises(RuntimeError):
        dvo.Oracle(command="/nonexistent/solver", out_dir=str(tmp_path / "x"))
