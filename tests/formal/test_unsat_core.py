"""(get-unsat-core) must always answer on stdout.

Verilator 5.x re-issues an unsatisfiable randomize() problem with named
assertions and then reads ONE line in reply to (get-unsat-core). Until
2026-09-30 dv-solve did not implement the command and wrote its error only to
the diagnostic log, so an unsat randomize() hung the simulation (B33).

The reply is the list of :named assertions in scope. The whole asserted set is
unsatisfiable, so that list is a valid (not necessarily minimal) core.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"


def _run(script: str, *args: str) -> list[str]:
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    p = subprocess.run([str(_EXE), "--interactive", *args], input=script,
                       capture_output=True, text=True, timeout=20)
    return p.stdout.splitlines()


_HDR = "(set-logic QF_BV)\n(declare-const x (_ BitVec 8))\n"


def test_core_lists_named_assertions_in_scope():
    out = _run(_HDR +
               "(assert (! (bvugt x #x0a) :named c0))\n"
               "(push 1)\n(assert (! (bvult x #x05) :named c1))\n"
               "(check-sat)\n(get-unsat-core)\n"
               "(pop 1)\n(assert (! (bvult x #x0c) :named c2))\n"
               "(assert (bvult x #x02))\n(check-sat)\n(get-unsat-core)\n")
    assert out == ["unsat", "(c0 c1)", "unsat", "(c0 c2)"]


def test_core_without_unsat_is_an_error_reply_not_silence():
    out = _run(_HDR + "(get-unsat-core)\n(assert (= x #x01))\n"
               "(check-sat)\n(get-unsat-core)\n")
    assert len(out) == 3
    assert out[0].startswith("(error") and out[2].startswith("(error")
    assert out[1] == "sat"


def test_reset_clears_names():
    out = _run(_HDR + "(assert (! (= x #x01) :named a))\n(reset)\n" + _HDR +
               "(assert (! (= x #x01) :named b))\n"
               "(assert (! (= x #x02) :named c))\n(check-sat)\n(get-unsat-core)\n")
    assert out == ["unsat", "(b c)"]


def test_verilator_unsat_randomize_shape_does_not_hang():
    # The exact command sequence Verilator 5.046 sends for an unsat randomize().
    script = ("(set-option :produce-models true)\n(set-logic QF_ABV)\n"
              "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n"
              "(declare-fun x () (_ BitVec 8))\n"
              "(assert (= #b1 (__Vbv (bvugt x #x0a))))\n"
              "(assert (= #b1 (__Vbv (bvult x #x05))))\n(check-sat)\n(reset)\n"
              "(set-option :produce-unsat-cores true)\n(set-logic QF_ABV)\n"
              "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n"
              "(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))\n"
              "(declare-fun x () (_ BitVec 8))\n"
              "(assert (! (= #b1 (__Vbv (bvugt x #x0a))) :named cons0))\n"
              "(assert (! (= #b1 (__Vbv (bvult x #x05))) :named cons1))\n"
              "(check-sat)\n(get-unsat-core) \n(reset)\n")
    assert _run(script, "--mode=verilator") == ["unsat", "unsat", "(cons0 cons1)"]
