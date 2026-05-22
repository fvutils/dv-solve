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
