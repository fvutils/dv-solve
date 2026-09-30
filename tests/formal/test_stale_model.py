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
