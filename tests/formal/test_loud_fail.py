"""Loud-but-in-sync failure mode.

When dv-solve-smt2 meets a construct it cannot translate (an unsupported
operator or sort), it must NOT exit: exiting closes the pipe and hangs a
driver (Verilator/yosys) blocked reading the next reply. Instead it stays in
the REPL, taints the assertion context, and the next (check-sat) answers
``unknown`` -- an honest, in-sync result the driver reads as a clean solver
error rather than a hang. The taint clears on (reset)/(reset-assertions) and
unwinds on (pop).

Usage:
    direnv exec . pytest tests/formal/test_loud_fail.py -v
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_DV_SOLVE = _REPO_ROOT / "build" / "dv-solve-smt2"


def _run(commands: str, timeout_s: float = 10.0) -> str:
    if not _DV_SOLVE.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    proc = subprocess.run(
        [str(_DV_SOLVE), "--interactive"],
        input=commands, capture_output=True, text=True, timeout=timeout_s,
    )
    return proc.stdout


def _results(stdout: str) -> list[str]:
    return [ln.strip() for ln in stdout.splitlines()
            if ln.strip() in ("sat", "unsat", "unknown")]


def test_unsupported_op_yields_unknown_and_stays_alive():
    """An unsupported operator taints -> check-sat is unknown, REPL recovers."""
    out = _run(
        "(set-logic QF_BV)"
        "(declare-fun x () (_ BitVec 8))"
        "(assert (= x (bvsdiv x #x02)))"   # bvsdiv: unsupported signed arith
        "(check-sat)"                       # -> unknown (tainted)
        "(reset)"                           # clears taint
        "(declare-fun y () (_ BitVec 8))"
        "(assert (= y #x05))"
        "(check-sat)"                       # -> sat (recovered)
        "(get-value (y))"
        "(exit)\n"
    )
    assert _results(out) == ["unknown", "sat"]
    assert "#b00000101" in out  # y == 5, so the process really recovered


def test_unsupported_sort_yields_unknown():
    """An unsupported sort taints the context the same way."""
    out = _run(
        "(set-logic QF_ABV)"
        "(declare-fun s () (_ BitVec 200))"  # width > 64: unsupported sort
        "(assert (= s s))"
        "(check-sat)"
        "(exit)\n"
    )
    assert _results(out) == ["unknown"]


def test_taint_unwinds_on_pop():
    """A bad assert inside a push must not poison the outer scope after pop."""
    out = _run(
        "(set-logic QF_BV)"
        "(declare-fun x () (_ BitVec 8))"
        "(assert (bvugt x #x00))"
        "(push 1)"
        "(assert (= x (bvsdiv x #x02)))"    # taints only the pushed scope
        "(check-sat)"                        # -> unknown
        "(pop 1)"                            # un-taints
        "(check-sat)"                        # -> sat (outer scope is clean)
        "(exit)\n"
    )
    assert _results(out) == ["unknown", "sat"]
