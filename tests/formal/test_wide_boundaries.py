"""Wrong answers at the 2^63 boundary and on self-comparisons, found by the
soundness campaign's wide mode on its first night (2026-10-02).

- B64: a Boolean ite whose branch compares two 64-bit constants folded the
  comparison as SIGNED in _bool_to_var: `0 <= 2^63 + k` was false.
- B65: ite_value_64 used signed min/max on an unsigned 64-bit domain stored
  as [0, -1]: `x <= ite(true, x, y)` was unsat through the builder.
- B62: `x <s x` lowered each side to its own `x ^ 2^(w-1)` auxiliary and
  cost the whole CDCL budget; same-variable compares now fold in the frontend.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from .soundness import doors
from .soundness.ir import Problem

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"


def _check(script: str) -> str:
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 not built")
    r = subprocess.run([str(_EXE)], input=script, capture_output=True, text=True, timeout=30)
    return r.stdout.split()[0]


@pytest.mark.parametrize("cond,expect", [
    ("(ite false true (bvule (_ bv0 64) (_ bv16068038427581503454 64)))", "sat"),
    ("(ite false true (bvult (_ bv1 64) (_ bv9223372036854775808 64)))", "sat"),
    ("(not (bvugt (_ bv1 64) (_ bv9223372036854775808 64)))", "sat"),
    ("(ite false true (bvugt (_ bv1 64) (_ bv9223372036854775808 64)))", "unsat"),
])
def test_constant_compare_at_2_63(cond, expect):
    assert _check(f"(set-logic QF_BV)(declare-const a (_ BitVec 1))(assert {cond})(check-sat)") == expect


@pytest.mark.parametrize("width", [8, 32, 63, 64])
def test_ite_value_on_wide_unsigned(width):
    x, y = ("var", "x", width), ("var", "y", width)
    p = Problem({"x": width, "y": width},
                [("bvule", "bool", x, ("ite", width, ("true",), x, y))])
    assert [o.got for o in doors.builder(p)] == ["sat", "sat"]


@pytest.mark.parametrize("cmp,expect", [("bvsgt", "unsat"), ("bvsge", "sat"),
                                        ("bvult", "unsat"), ("bvule", "sat")])
def test_self_compare_is_immediate(cmp, expect):
    assert _check(f"(set-logic QF_BV)(declare-const x (_ BitVec 63))(assert ({cmp} x x))"
                  "(check-sat)") == expect
