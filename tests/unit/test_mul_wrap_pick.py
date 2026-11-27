"""A wrapping product with one factor fixed: x drawn from the product's range.

    rand int unsigned prescale[2], ticks[2];
    solve prescale before ticks;
    foreach (ticks[i]) ticks[i] * (prescale[i] + 1) <= 5000;

(Verilator's t_constraint_solve_before_array, class Mul.) Once prescale is
decided, ticks * c (mod 2^32) <= 5000 holds for about 5001 of 2^32 ticks,
spread over the whole range: the product wraps, so its bounds teach ticks
nothing. The CDCL search drew ticks uniformly, refuted draw after draw, and
gave up at the time limit: "could not solve" on every seed. The solve order
keeps the problem on CDCL (bit-blast cannot honour it).

The picker now draws the product r in its bounds, then ticks from it:
c = 2^k * odd, r a multiple of 2^k, ticks = (r >> k) * odd^-1 mod 2^(32-k)
with its top k bits drawn (_pick_mul_inverse in dvs_search.c).
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.handle import RANDOMIZE_OK, SolveHandle
from dv_solve.problem import BIN_ADD, BIN_EQ, BIN_LTE, BIN_MUL


def _mul_handle(n, width=32, bound=5000, prescale=None, order=True):
    """ticks = vars 0..n-1, prescale = n..2n-1."""
    b = SolveProblemBuilder()
    m = (1 << width) - 1
    for i in range(2 * n):
        b.add_var(i, width=width, is_signed=False, lo=0, hi=m)
    for i in range(n):
        p1 = b.expr_binary(BIN_ADD, b.expr_var(n + i), b.expr_const(1, width=width))
        prod = b.expr_binary(BIN_MUL, b.expr_var(i), p1)
        b.add_constraint(b.expr_binary(BIN_LTE, prod, b.expr_const(bound, width=width)))
        if prescale is not None:
            b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(n + i),
                                           b.expr_const(prescale, width=width)))
        if order:
            b.set_var_order(n + i, 1)    # solve prescale before ticks
            b.set_var_order(i, 2)
    return SolveHandle(b.finalize_bytes(), vars=[(width, False)] * (2 * n))


def _seed(i):
    return (0x9E3779B97F4A7C15 * (i + 1)) & (2**64 - 1)


@pytest.mark.parametrize("n", [1, 2])
def test_solve_before_wrapping_mul(n):
    h = _mul_handle(n)
    ticks = set()
    for i in range(40):
        rc, vals, _ = h.randomize(_seed(i))
        assert rc == RANDOMIZE_OK, f"seed {i}: rc={rc}"
        for j in range(n):
            assert (vals[j] * (vals[n + j] + 1)) & 0xFFFFFFFF <= 5000, vals
            ticks.add(vals[j])
    assert len(ticks) > 30          # not ticks == 0 every time


@pytest.mark.parametrize("prescale", [5, 7, 63])
def test_wrapping_mul_takes_every_solution(prescale):
    """8 bits, c = prescale + 1 (6 = 2 * 3, 8 = 2^3, 64 = 2^6): every x with
    x * c mod 256 <= 20 comes up."""
    c = prescale + 1
    want = {x for x in range(256) if (x * c) & 0xFF <= 20}
    h = _mul_handle(1, width=8, bound=20, prescale=prescale, order=False)
    got = []
    for i in range(30 * len(want)):
        rc, vals, _ = h.randomize(_seed(i))
        assert rc == RANDOMIZE_OK
        got.append(vals[0])
    assert set(got) == want, sorted(set(got) ^ want)
