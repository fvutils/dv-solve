"""A model must come from the solve that produced the current answer.

B36 (fixed 2026-09-30): the bit-blast solver object outlives its answer -- the
Verilator identity cache carries it across (reset) -- and model readback used it
whenever it existed. A bit-blast solve, then (reset), then a CDCL-routed solve
answered `sat` and printed the OLD bit-blast values, which violate the new
constraints.
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
                       capture_output=True, text=True, timeout=30)
    return p.stdout.splitlines()


_BB_THEN_CDCL = ("(set-logic QF_ABV)\n(declare-fun x () (_ BitVec 8))\n"
                 "(assert (= x #x11))\n(check-sat)\n(get-value (x))\n(reset)\n"
                 "(set-logic QF_BV)\n(declare-fun x () (_ BitVec 8))\n"
                 "(assert (= x #x22))\n(check-sat)\n(get-value (x))\n")


@pytest.mark.parametrize("mode", [[], ["--mode=verilator"]], ids=["default", "verilator"])
def test_cdcl_after_bitblast_and_reset_reports_its_own_model(mode):
    assert _run(_BB_THEN_CDCL, *mode) == [
        "sat", "((x #b00010001))", "sat", "((x #b00100010))"]


def test_verilator_check_sat_assuming_reuses_previous_model():
    out = _run("(set-logic QF_ABV)\n(declare-fun x () (_ BitVec 8))\n"
               "(declare-fun p () Bool)\n(assert (bvugt x #x10))\n"
               "(check-sat)\n(get-value (x))\n(check-sat-assuming (p))\n"
               "(get-value (x))\n", "--mode=verilator")
    assert out[0] == "sat" and out[2] == "sat"
    assert out[1] == out[3]                       # same model, re-announced
    assert int(out[1].split("#b")[1].rstrip("))"), 2) > 0x10


def _bits(line: str) -> int:
    return int(line.split("#b")[1].rstrip("))"), 2)


def test_verilator_check_sat_assuming_reuses_model_past_assumption_definitions():
    # Verilator's first randomize() pass: define one Bool per pinned bit, then
    # assume them. The definitions don't constrain x, so one solve serves.
    out = _run("(set-logic QF_ABV)\n(declare-fun x () (_ BitVec 8))\n"
               "(assert (bvugt x #x10))\n(check-sat)\n(get-value (x))\n"
               "(declare-fun a0 () Bool)\n"
               "(assert (= a0 (= ((_ extract 0 0) x) #b1)))\n"
               "(check-sat-assuming (a0))\n(get-value (x))\n", "--mode=verilator")
    assert out[0] == "sat" and out[2] == "sat"
    assert out[1] == out[3]


def test_verilator_check_sat_assuming_after_blocking_assert_solves_again():
    # Verilator 5.052's enumeration blocks each answer, then asks for another.
    # Re-announcing the blocked model was a wrong model, and made every later
    # randomize() call return the same values.
    script = ("(set-logic QF_ABV)\n(declare-fun x () (_ BitVec 8))\n"
              "(assert (bvult x #x04))\n(check-sat)\n(get-value (x))\n")
    seen = []
    for n in range(4):
        out = _run(script, "--mode=verilator")
        assert out[-2] == "sat"
        v = _bits(out[-1])
        assert v < 4 and v not in seen
        seen.append(v)
        script += (f"(assert (not (= x #b{v:08b})))\n"
                   f"(declare-fun d{n} () Bool)\n"
                   f"(assert (= d{n} (not (= x #b{v:08b}))))\n"
                   f"(check-sat-assuming (d{n}))\n(get-value (x))\n")
    out = _run(script, "--mode=verilator")
    assert out[-2] == "unsat"                     # every value blocked
