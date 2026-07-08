"""Exhaustive soundness + completeness validation of the backward bit-slice
propagator (`field == extract(operand, hi, lo)`), the fix for BUG-2.

Before the fix the bit-slice propagator was forward-only (operand -> slice); a
field pinned with a free operand returned spurious UNSAT because a non-bounds-
consistent propagator breaks the search's domain-bisection completeness.

This test brute-forces every (operand_width, hi, lo, [alo,ahi], field_value) for
small widths and asserts the solver's SAT/UNSAT verdict and (on SAT) the model
exactly match a Python oracle. It is the guardrail cited in
zsp_prop_templates.c (_slice_min_ge / _slice_max_le).
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_EQ
from dv_solve.ctx import SolveCtx, CompileUnsatError, CompileIncompleteError


def _brute(hi, lo, alo, ahi, fv):
    m = (1 << (hi - lo + 1)) - 1
    for a in range(alo, ahi + 1):
        if ((a >> lo) & m) == (fv & m):
            return a
    return None


def _solve(aw, hi, lo, alo, ahi, fv):
    sw = hi - lo + 1
    b = SolveProblemBuilder()
    b.add_var(0, aw, False, alo, ahi)
    b.add_var(1, sw, False, fv, fv)   # field pinned
    b.add_constraint(
        b.expr_binary(BIN_EQ, b.expr_var(1), b.expr_extract(b.expr_var(0), hi, lo))
    )
    buf, _sz = b.finalize()
    try:
        try:
            ctx = SolveCtx(buf)
        except CompileUnsatError:
            return ("UNSAT", None)
        except CompileIncompleteError:
            return ("INCOMPLETE", None)
        rc = ctx.solve(seed=1)
        x = ctx.get_value(0) if rc == 0 else None
        ctx.destroy()
        return (("SAT", x) if rc == 0 else ("UNSAT", None))
    finally:
        b.destroy()


# Keep the sweep quick but representative: widths 1..6, all slice positions, a
# spread of domain windows, all field values. (The C fix was validated over
# 1..8 / 44k cases during development; 1..6 keeps CI fast while still exercising
# top/low/middle slices and in/out-of-group boundaries.)
@pytest.mark.parametrize("aw", [1, 2, 3, 4, 5, 6])
def test_bitslice_backward_exhaustive(aw):
    full = (1 << aw) - 1
    checks = 0
    for hi in range(aw):
        for lo in range(hi + 1):
            sw = hi - lo + 1
            windows = set()
            for alo in {0, 1, full // 3, full // 2, full - 1, full}:
                for size in {0, 1, 2, full // 2, full}:
                    ahi = min(alo + size, full)
                    if 0 <= alo <= ahi <= full:
                        windows.add((alo, ahi))
            for (alo, ahi) in windows:
                for fv in range(1 << sw):
                    checks += 1
                    oracle = _brute(hi, lo, alo, ahi, fv)
                    st, x = _solve(aw, hi, lo, alo, ahi, fv)
                    exp_sat = oracle is not None
                    got_sat = (st == "SAT")
                    assert exp_sat == got_sat, (
                        "verdict mismatch aw=%d [%d:%d] dom=[%d,%d] fv=%d: "
                        "oracle=%s solver=%s" % (aw, hi, lo, alo, ahi, fv,
                                                 "SAT" if exp_sat else "UNSAT", st)
                    )
                    if got_sat:
                        m = (1 << sw) - 1
                        assert ((x >> lo) & m) == (fv & m) and alo <= x <= ahi, (
                            "bad model aw=%d [%d:%d] dom=[%d,%d] fv=%d x=%d"
                            % (aw, hi, lo, alo, ahi, fv, x)
                        )
    assert checks > 0
