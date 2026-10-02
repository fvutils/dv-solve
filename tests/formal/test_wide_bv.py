"""Differential coverage for wide (>64-bit) bit-vectors (Phases W1 + W2).

Runs each wide-BV probe (see ``generate_wide_smt2.py``) through both dv-solve
engines and z3, enforcing:

  * **Soundness (always):** neither dv-solve engine may *disagree* with z3.
    A dv-solve answer must be z3's answer or ``unknown`` (an honest punt on a
    construct not yet represented). A hard sat-vs-unsat disagreement is a
    soundness bug.
  * **Coverage (supported shapes):** every fixture except ``*_arrunk`` must be
    answered by each engine and match z3. W1 made wide *variables* answerable;
    W2 does the same for wide *literals* (``#x``/``#b`` wider than 64 bits and
    ``(_ bvN W)`` with N >= 2^64), so ``w128_hexmask_eq_wideunk`` -- named
    when W1 still punted on it -- is now a coverage fixture too.
  * ``*_arrunk`` fixtures index a 128-bit-address array with a wide literal
    (correctness-gap Step 3); they are checked for soundness only.

Also checks wide model *readback*: get-value of wide vars, define-funs and
array elements must print the full value (never a 64-bit truncation),
cross-checked against z3's model values.

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
    if not smt2_file.stem.endswith("_arrunk"):
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


def _bv_int(tok: str) -> int:
    """`#b0101` / `#x1f` -> int."""
    if tok.startswith("#b"):
        return int(tok[2:], 2)
    if tok.startswith("#x"):
        return int(tok[2:], 16)
    raise ValueError(tok)


def _get_values(out: str) -> list[int]:
    """All BV literal values in a get-value response, in order."""
    import re
    return [_bv_int(t) for t in re.findall(r"#[bx][0-9a-fA-F]+", out)]


# (problem, get-value terms) -- each constraint set pins every queried term to
# a unique value, so dv-solve's model must print exactly z3's.
_GV_CASES = {
    "var_hi_limb": (
        "(declare-const x (_ BitVec 128))"
        "(assert (= x #x8000000000000001ffffffffffffffff))", "(x)"),
    "var_lo_limb_only": (
        "(declare-const x (_ BitVec 128))"
        "(assert (= x #x0000000000000000ffffffffffffffff))", "(x)"),
    "bvN_big": (
        "(declare-const x (_ BitVec 128))"
        "(assert (= x (_ bv340282366920938463463374607431768211454 128)))", "(x)"),
    "add_carry": (
        "(declare-const x (_ BitVec 128))"
        "(assert (= (bvadd x #x0000000000000000ffffffffffffffff)"
        " #x0123456789abcdeffedcba9876543210))", "(x)"),
    "odd_width_100": (
        "(declare-const x (_ BitVec 100))"
        "(assert (= (bvxor x #xfffffffffffffffffffffffff) #x0000000000000000000000001))",
        "(x)"),
    "define_fun_wide": (
        "(declare-const x (_ BitVec 128))"
        "(define-fun k () (_ BitVec 128) #x0123456789abcdeffedcba9876543210)"
        "(define-fun hi () (_ BitVec 64) ((_ extract 127 64) x))"
        "(assert (= x (bvadd k #x00000000000000010000000000000000)))", "(x k hi)"),
    "define_fun_concat": (
        "(declare-const x (_ BitVec 64))(declare-const y (_ BitVec 64))"
        "(define-fun c () (_ BitVec 128) (concat x y))"
        "(assert (= (concat x y) #xffffffffffffffff0000000000000001))", "(c x y)"),
    "array_elem_wide": (
        "(declare-const a (Array (_ BitVec 4) (_ BitVec 128)))"
        "(assert (= (select a #x3) #xffff0000000000000000000000000001))",
        "((select a #x3))"),
}


@pytest.mark.parametrize("case", sorted(_GV_CASES))
def test_wide_literal_get_value(case, z3_oracle):
    """get-value after a wide-literal constraint matches z3's model exactly."""
    binary = _BUILD_DIR / "dv-solve-smt2"
    if not binary.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    body, terms = _GV_CASES[case]
    logic = "QF_ABV" if "Array" in body else "QF_BV"
    smt2 = (f"(set-logic {logic})\n(set-option :produce-models true)\n{body}\n"
            f"(check-sat)\n(get-value {terms})\n")
    z3 = subprocess.run(["z3", "-in"], input=smt2, capture_output=True,
                        text=True, timeout=TIMEOUT_S).stdout
    assert z3.splitlines()[0] == "sat", f"z3: {z3!r}"
    want = _get_values(z3.split("\n", 1)[1])
    for env in ({}, {"DV_ENGINE": "bitblast"}):
        import os
        out = subprocess.run([str(binary), "/dev/stdin"], input=smt2,
                             capture_output=True, text=True, timeout=TIMEOUT_S,
                             env={**os.environ, **env}).stdout
        assert out.splitlines()[0] == "sat", f"{env}: {out!r}"
        got = _get_values(out.split("\n", 1)[1])
        # Array get-value echoes the select index (`#x3`) as the key: compare
        # the element values only.
        if "select" in terms:
            got, want_v = got[1::2], want[1::2]
        else:
            want_v = want
        assert got == want_v, f"{case} {env}: dv={out!r} z3={z3!r}"
