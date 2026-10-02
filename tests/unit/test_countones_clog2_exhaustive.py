"""$countones and $clog2 over every small operand range, against brute force.

The $countones propagator once took the bits set in both ends of the operand's
range as set in every value: for x in [3, 7] it decided at least two bits were
set, though 4 (0b100) has one, and `$countones(x) == 1` came back unsat. It
also bounded a signed operand's value as if it were unsigned. These sweeps
check every range of every small width, signed and unsigned, against the
values themselves.
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SOLVE_OK, SOLVE_UNSAT, CompileUnsatError, SolveCtx


def _pattern(v: int, width: int) -> int:
    return v & ((1 << width) - 1)


def _countones(v: int, width: int) -> int:
    return bin(_pattern(v, width)).count("1")


def _clog2(v: int, width: int) -> int:
    u = _pattern(v, width)                 # $clog2 reads its argument unsigned
    return 0 if u <= 1 else (u - 1).bit_length()


def _solve(build, n_vars):
    try:
        ctx = SolveCtx(build())
    except CompileUnsatError:
        return SOLVE_UNSAT, None
    with ctx:
        rc = ctx.solve(seed=1)
        return rc, ([ctx.get_value(i) for i in range(n_vars)] if rc == SOLVE_OK else None)


@pytest.mark.parametrize("fn", ["countones", "clog2"])
@pytest.mark.parametrize("signed", [False, True])
@pytest.mark.parametrize("width", [1, 2, 3, 4])
def test_every_range_and_result(fn: str, signed: bool, width: int) -> None:
    ref = _countones if fn == "countones" else _clog2
    lo_v, hi_v = (-(1 << (width - 1)), (1 << (width - 1)) - 1) if signed else (0, (1 << width) - 1)
    for lo in range(lo_v, hi_v + 1):
        for hi in range(lo, hi_v + 1):
            for k in range(0, width + 2):
                def build():
                    b = SolveProblemBuilder()
                    b.add_var(0, width=width, is_signed=signed, lo=lo, hi=hi)
                    b.add_var(1, width=8, is_signed=False, lo=k, hi=k)
                    op = b.expr_countones if fn == "countones" else b.expr_clog2
                    b.add_constraint(op(b.expr_var(1), b.expr_var(0)))
                    return b.finalize()[0]
                want = [x for x in range(lo, hi + 1) if ref(x, width) == k]
                rc, vals = _solve(build, 2)
                case = (fn, "signed" if signed else "unsigned", width, (lo, hi), k)
                if want:
                    assert rc == SOLVE_OK, ("wrong unsat", case)
                    assert vals[0] in want, ("wrong model", case, vals)
                else:
                    assert rc != SOLVE_OK, ("wrong sat", case, vals)
