"""Incremental SMT-LIB2 protocol test.

Drives ``dv-solve-smt2`` over a pipe with the BMC unroll pattern:
push -> declare/assert new step -> check-sat -> pop.  Validates that
the solver gives correct answers across multiple push/pop cycles
and across check-sat-assuming.

Usage:
    direnv exec . pytest tests/formal/test_incremental_protocol.py -v
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_DV_SOLVE = _REPO_ROOT / "build" / "dv-solve-smt2"


def _run_interactive(commands: str, timeout_s: float = 10.0) -> tuple[str, str, int]:
    """Pipe `commands` into dv-solve-smt2 interactive mode."""
    if not _DV_SOLVE.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    proc = subprocess.run(
        [str(_DV_SOLVE), "--interactive"],
        input=commands,
        capture_output=True,
        text=True,
        timeout=timeout_s,
    )
    return proc.stdout, proc.stderr, proc.returncode


def _results(stdout: str) -> list[str]:
    """Extract the sat/unsat/unknown lines from solver stdout."""
    return [
        line.strip()
        for line in stdout.splitlines()
        if line.strip() in ("sat", "unsat", "unknown")
    ]


def test_basic_push_pop():
    cmds = """
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(assert (bvuge x (_ bv42 8)))
(check-sat)
(push 1)
(assert (bvult x (_ bv50 8)))
(check-sat)
(pop 1)
(push 1)
(assert (bvuge x (_ bv200 8)))
(check-sat)
(pop 1)
(check-sat)
(exit)
"""
    out, _, _ = _run_interactive(cmds)
    assert _results(out) == ["sat", "sat", "sat", "sat"]


def test_push_pop_with_unsat():
    cmds = """
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(assert (bvuge x (_ bv200 8)))
(check-sat)
(push 1)
(assert (bvult x (_ bv50 8)))
(check-sat)
(pop 1)
(check-sat)
(exit)
"""
    out, _, _ = _run_interactive(cmds)
    assert _results(out) == ["sat", "unsat", "sat"]


def test_check_sat_assuming():
    cmds = """
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(declare-const p Bool)
(assert (bvuge x (_ bv42 8)))
(check-sat)
(check-sat-assuming (p))
(check-sat-assuming ((not p)))
(exit)
"""
    out, _, _ = _run_interactive(cmds)
    assert _results(out) == ["sat", "sat", "sat"]


def test_get_model():
    cmds = """
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(assert (bvuge x (_ bv42 8)))
(check-sat)
(get-model)
(exit)
"""
    out, _, _ = _run_interactive(cmds)
    assert "sat" in out
    assert "define-fun x" in out


def test_echo_and_get_info():
    cmds = """
(echo "hello")
(get-info :name)
(get-info :version)
(exit)
"""
    out, _, _ = _run_interactive(cmds)
    assert '"hello"' in out
    assert "dv-solve-smt2" in out


def test_bmc_style_unroll():
    """Drive a BMC-style unroll: push, assert per-step, check, pop, repeat."""
    cmds = """
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(assert (bvuge x (_ bv1 8)))
(check-sat)
(push 1)
(assert (bvule x (_ bv5 8)))
(check-sat)
(pop 1)
(push 1)
(assert (bvule x (_ bv10 8)))
(check-sat)
(pop 1)
(push 1)
(assert (bvuge x (_ bv200 8)))
(assert (bvule x (_ bv10 8)))
(check-sat)
(pop 1)
(check-sat)
(exit)
"""
    out, _, _ = _run_interactive(cmds)
    res = _results(out)
    assert res == ["sat", "sat", "sat", "unsat", "sat"], res


# ---------------------------------------------------------------------------
# B14 regression: an (assert) issued AFTER a (check-sat) must reach the solver.
#
# The bitblast engine reads fe->problem directly and, unlike CDCL, had no
# _flush_aux equivalent -- so post-first-solve asserts accumulated in the
# builder and were silently dropped, yielding a wrong `sat` on a problem the
# new assert had made unsatisfiable.  Found while evaluating Verilator PR#8042
# (UniGen2), whose model-enumeration loop depends entirely on this working.
# See docs/solver_bug_backlog.md B14.
# ---------------------------------------------------------------------------

def _run_with_args(commands: str, args: list[str], timeout_s: float = 20.0) -> str:
    if not _DV_SOLVE.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    proc = subprocess.run(
        [str(_DV_SOLVE), "--interactive", *args],
        input=commands, capture_output=True, text=True, timeout=timeout_s,
    )
    return proc.stdout


_B14_BODY = """(declare-fun a () (_ BitVec 3))
(assert (bvult a #b101))
(check-sat)
(assert (bvugt a #b110))
(check-sat)
(exit)
"""


@pytest.mark.parametrize("logic", ["QF_BV", "QF_ABV"])
@pytest.mark.parametrize("engine", ["cdcl", "bitblast", "auto"])
def test_b14_incremental_assert_reaches_solver(logic: str, engine: str) -> None:
    """`a < 5` then `a > 6` is unsat. Dropping the 2nd assert gives a wrong sat.

    Must hold on EVERY logic/engine pairing: real Verilator emits QF_ABV, which
    force-routes to bitblast -- the combination that was broken.
    """
    out = _run_with_args(f"(set-logic {logic})\n" + _B14_BODY, [f"--engine={engine}"])
    assert _results(out) == ["sat", "unsat"], (logic, engine, out)


def test_b14_blocking_clause_enumeration_yields_distinct_models() -> None:
    """The UniGen2 BSAT loop: check-sat / get-value / block, repeated.

    Before the fix the blocking clause was dropped and the solver returned the
    same model forever.  Enumerate the full 5-element domain of `a` and require
    every model to be distinct and the 6th query to be unsat.
    """
    cmds = ["(set-logic QF_ABV)",
            "(declare-fun a () (_ BitVec 3))",
            "(declare-fun b () (_ BitVec 3))",
            "(assert (bvult a #b101))",
            "(assert (= b #b000))"]
    # We cannot know offline which model comes back, so instead block all 5
    # legal values of `a` one at a time and require sat*5 then unsat -- which
    # only holds if every blocking clause actually reached the solver.
    for v in range(5):
        cmds += ["(check-sat)", "(get-value (a b))"]
        cmds.append(f"(assert (not (and (= a #b{v:03b}) (= b #b000))))")
    cmds += ["(check-sat)", "(exit)"]
    out = _run_with_args("\n".join(cmds) + "\n", [])
    res = _results(out)
    # The blocking clauses remove one candidate each; after all 5 the domain of
    # `a` under `a < 5` is exhausted.
    assert res == ["sat"] * 5 + ["unsat"], res
