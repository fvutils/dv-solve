"""Differential coverage for wide (>64-bit) bit-vectors (Phase W1).

Runs each wide-BV probe (see ``generate_wide_smt2.py``) through both dv-solve
engines and z3, enforcing:

  * **Soundness (always):** neither dv-solve engine may *disagree* with z3.
    A dv-solve answer must be z3's answer or ``unknown`` (an honest punt on a
    construct W1 doesn't yet represent). A hard sat-vs-unsat disagreement is a
    soundness bug.
  * **Coverage (supported shapes):** for every fixture NOT marked ``_wideunk``,
    each engine must actually answer and match z3 — this is the real win: wide
    BVs used to be a blanket ``unknown``.

``*_wideunk`` fixtures use a ``#x…`` literal whose *value* exceeds 64 bits;
the lexer truncates it, so W1 must answer ``unknown`` (never a wrong result).
The multi-limb literal path that would answer these is Phase W2.

Also checks wide model *readback*: a 65-bit wrap gives x = 2**65-1, and
get-value must emit the full 65-bit pattern (not a 64-bit truncation).

Run:  direnv exec . pytest tests/formal/test_wide_bv.py -v
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from .harness.dv_solve_smt2_solver import (
    DvSolveSMT2Solver,
    DvSolveSMT2BBSolver,
    _BUILD_DIR,
)
from .harness.z3_solver import Z3Solver

WIDE_DIR = Path(__file__).resolve().parent / "smt2" / "wide"
TIMEOUT_S = 10.0
_NON_ANSWERS = {"unknown", "timeout", "error"}

_DV_CDCL = DvSolveSMT2Solver()
_DV_BB = DvSolveSMT2BBSolver()
_Z3 = Z3Solver()


def _files() -> list[Path]:
    return sorted(WIDE_DIR.glob("*.smt2")) if WIDE_DIR.is_dir() else []


_FILES = _files()


@pytest.fixture(scope="module")
def z3_oracle():
    if not _Z3.is_available():
        pytest.skip("z3 not on PATH; cannot cross-check")
    return _Z3


@pytest.mark.parametrize("smt2_file", _FILES, ids=[f.stem for f in _FILES])
def test_wide_bv(smt2_file, z3_oracle):
    if not _DV_CDCL.is_available():
        pytest.skip("dv-solve-smt2 binary not built")

    z3 = z3_oracle.solve(smt2_file, timeout_s=TIMEOUT_S).result
    if z3 not in {"sat", "unsat"}:
        pytest.skip(f"z3 returned non-answer: {z3}")

    cdcl = _DV_CDCL.solve(smt2_file, timeout_s=TIMEOUT_S).result
    bb = _DV_BB.solve(smt2_file, timeout_s=TIMEOUT_S).result

    # Soundness: no hard disagreement with z3 on either engine.
    for eng, res in (("cdcl", cdcl), ("bitblast", bb)):
        if res in {"sat", "unsat"}:
            assert res == z3, (
                f"SOUNDNESS: dv-solve ({eng}) says '{res}', z3 says '{z3}' "
                f"on {smt2_file.stem}"
            )

    # Coverage: supported (non-wideunk) shapes must be answered, matching z3.
    if not smt2_file.stem.endswith("_wideunk"):
        assert cdcl not in _NON_ANSWERS, (
            f"COVERAGE: dv-solve (cdcl) punted ('{cdcl}') on {smt2_file.stem}"
        )
        assert bb not in _NON_ANSWERS, (
            f"COVERAGE: dv-solve (bitblast) punted ('{bb}') on {smt2_file.stem}"
        )


def test_wide_readback_65bit():
    """A 65-bit wrap forces x = 2**65-1; get-value must emit all 65 bits."""
    binary = _BUILD_DIR / "dv-solve-smt2"
    if not binary.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    smt2 = (
        "(set-logic QF_BV)\n"
        "(declare-const x (_ BitVec 65))\n"
        "(assert (= (bvadd x (_ bv1 65)) (_ bv0 65)))\n"
        "(check-sat)\n(get-value (x))\n"
    )
    out = subprocess.run([str(binary), "/dev/stdin"], input=smt2,
                         capture_output=True, text=True, timeout=TIMEOUT_S).stdout
    assert "sat" in out.splitlines()[0]
    # 2**65 - 1 == sixty-five 1 bits.
    assert "#b" + "1" * 65 in out, f"wide readback wrong: {out!r}"
