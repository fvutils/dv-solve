"""$countones(x) == k and $onehot(x) through dvs_randomize(): x varies.

The values with k bits set are a sliver of a wide x's range (33 of 2^33 for
k == 1). The search drew x uniformly from its bounds, refuted the draw, and
narrowed toward a bound, so every seed ended on the same value
(0x1_0000_0000 for a 33-bit `$onehot`, Verilator's t_constraint_countones);
for 2 <= k <= 31 the search gave up and bit-blast took over, with few values.
The pick domain now draws x by its popcount (_pd_draw_pc in dvs_search.c).

Verilator lowers `$countones(x) == k` to an aux r, `countones(r, x)` and
`r == k`, and `$onehot0(x)` to `r <= 1`.
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.handle import RANDOMIZE_OK, SolveHandle
from dv_solve.problem import BIN_ADD, BIN_EQ, BIN_LTE, VAR_AUX

_XW = 33


def _draws(h, n, vid=0, seed0=1):
    got = []
    for i in range(n):
        rc, vals, _ = h.randomize(seed0 + 7919 * i)
        assert rc == RANDOMIZE_OK, f"call {i}: rc={rc}"
        got.append(vals[vid])
    return got


def _popcount_handle(k, op=BIN_EQ, const_count=False):
    b = SolveProblemBuilder()
    b.add_var(0, width=_XW, is_signed=False, lo=0, hi=(1 << _XW) - 1)
    if const_count:
        b.add_constraint(b.expr_countones(b.expr_const(k, width=32), b.expr_var(0)))
        return SolveHandle(b.finalize_bytes(), vars=[(_XW, False)])
    b.add_var(1, width=32, is_signed=False, lo=0, hi=(1 << 32) - 1)
    b.set_var_kind(1, VAR_AUX)
    b.add_constraint(b.expr_countones(b.expr_var(1), b.expr_var(0)))
    b.add_constraint(b.expr_binary(op, b.expr_var(1), b.expr_const(k, width=32)))
    return SolveHandle(b.finalize_bytes(), vars=[(_XW, False), None])


@pytest.mark.parametrize("k", [1, 32])
@pytest.mark.parametrize("const_count", [False, True])
def test_countones_k_takes_every_value(k, const_count):
    """33 values have 1 (or 32) of 33 bits set: 300 draws take them all."""
    got = _draws(_popcount_handle(k, const_count=const_count), 300)
    assert all(bin(x).count("1") == k for x in got), got
    assert len(set(got)) == _XW, sorted(set(got))


@pytest.mark.parametrize("k", [2, 3, 16, 31])
def test_countones_k_varies(k):
    """C(33, k) >= 528 values: 200 draws are nearly all different."""
    got = _draws(_popcount_handle(k), 200)
    assert all(bin(x).count("1") == k for x in got), got
    assert len(set(got)) >= 150, len(set(got))


def test_countones_bit_positions_uniform():
    """$onehot over 33 bits: each bit is the set one about 1/33 of the time."""
    got = _draws(_popcount_handle(1), 1650)
    for bit in range(_XW):
        n = sum(1 for x in got if x == 1 << bit)
        assert 20 <= n <= 85, (bit, n)


def test_onehot0_takes_zero_and_onehot():
    """$onehot0: countones(x) <= 1. 0 is 1 of 34 values."""
    got = _draws(_popcount_handle(1, op=BIN_LTE), 400)
    assert all(bin(x).count("1") <= 1 for x in got), got
    assert 0 in got
    assert len(set(got)) == _XW + 1, sorted(set(got))


def test_countones_of_sum_varies():
    """rand bit [5:0] x; rand bit [2:0] y; $countones(x + 6'(y)) == 2
    (Verilator's t_constraint_countones Rand2): an aux holds the sum."""
    b = SolveProblemBuilder()
    b.add_var(0, width=6, is_signed=False, lo=0, hi=63)    # x
    b.add_var(1, width=3, is_signed=False, lo=0, hi=7)     # y
    b.add_var(2, width=6, is_signed=False, lo=0, hi=63)    # x + y
    b.add_var(3, width=10, is_signed=False, lo=0, hi=1023)  # countones
    b.set_var_kind(2, VAR_AUX)
    b.set_var_kind(3, VAR_AUX)
    s = b.expr_binary(BIN_ADD, b.expr_var(0), b.expr_extend(b.expr_var(1), 3, 6))
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(2), s))
    b.add_constraint(b.expr_countones(b.expr_var(3), b.expr_var(2)))
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(3), b.expr_const(2, width=10)))
    h = SolveHandle(b.finalize_bytes(), vars=[(6, False), (3, False), None, None])
    pairs = []
    for i in range(100):
        rc, vals, _ = h.randomize(11 + 7919 * i)
        assert rc == RANDOMIZE_OK
        x, y = vals[0], vals[1]
        assert bin((x + y) & 63).count("1") == 2, (x, y)
        pairs.append((x, y))
    assert len(set(pairs)) >= 50, len(set(pairs))
