"""Distributions whose values, weights or guard read RAND variables.

`x dist { y :/ 90, 9 :/ 10 }` with `y` rand (t_constraint_dist_nonconst),
`x dist { [y - g : y] := 1, 5 := 1 }` (t_constraint_inside_impl_mixwidth),
`arr[i] dist { 0 :/ wz, [1:15] :/ wn }` with wz, wn rand
(t_randomize_dist_foreach).

A variable a distribution reads is solved before the distribution's
variable, and the distribution is evaluated with its value when the search
decides that variable: one without a solve order of its own is put in an
earlier stage (an implicit `solve y before x`); one with an explicit stage
must already be earlier. The front end adds the membership (`x inside` the
entries) as an ordinary constraint, as it does for any dist_ex.
"""
from __future__ import annotations

from collections import Counter

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.handle import RANDOMIZE_ERROR, RANDOMIZE_OK, SolveHandle
from dv_solve.problem import BIN_ADD, BIN_EQ, BIN_GT, BIN_MUL, BIN_OR, BIN_SUB, VAR_STATE


def _c(b, v, w=8):
    return b.expr_const(v, width=w)


def _rand_items(order_y=0, order_x=0):
    """rand bit [7:0] x, y; y inside {[1:2]}; x dist { y :/ 90, 9 :/ 10 }."""
    b = SolveProblemBuilder()
    b.add_var(0, 8, False, 0, 255)   # x
    b.add_var(1, 8, False, 0, 255)   # y
    if order_y:
        b.set_var_order(1, order_y)
    if order_x:
        b.set_var_order(0, order_x)
    b.add_constraint(b.expr_in_range(b.expr_var(1), _c(b, 1), _c(b, 2)))
    b.add_constraint(b.expr_binary(BIN_OR, b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_var(1)),
                                   b.expr_binary(BIN_EQ, b.expr_var(0), _c(b, 9))))
    b.add_dist_ex(0, [{"lo": b.expr_var(1), "weight": b.expr_const(90), "is_per_value": False},
                      {"lo": _c(b, 9), "weight": b.expr_const(10), "is_per_value": False}])
    return b


@pytest.mark.parametrize("order", [(0, 0), (1, 0), (1, 2)])
def test_value_reads_a_rand_variable(order):
    """x == y 90% of the time, x == 9 10%; with no order, and with
    `solve y before x` given."""
    h = SolveHandle(_rand_items(*order).finalize_bytes(), vars=[(8, False), (8, False)])
    n = 1000
    got = Counter()
    ys = Counter()
    for i in range(n):
        rc, v, _ = h.randomize(31 * i + 7)
        assert rc == RANDOMIZE_OK
        assert v[0] == v[1] or v[0] == 9
        got["y" if v[0] == v[1] else "9"] += 1
        ys[v[1]] += 1
    assert 0.85 * n < got["y"] < 0.95 * n, got
    assert 0.4 * n < ys[1] < 0.6 * n, ys      # y itself stays uniform


def test_range_bounds_read_rand_variables():
    """x dist { [y - g : y] := 1, 5 := 1 }, y == 0x100, g in [1:10]: each of
    the g + 2 values has weight 1, so x == 5 has probability 1 / (g + 2)."""
    b = SolveProblemBuilder()
    b.add_var(0, 16, False, 0, 0xFFFF)   # x
    b.add_var(1, 16, False, 0, 0xFFFF)   # y
    b.add_var(2, 16, False, 0, 0xFFFF)   # g
    c = lambda v: b.expr_const(v, width=16)
    b.add_constraint(b.expr_in_range(b.expr_var(2), c(1), c(10)))
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1), c(0x100)))
    lo = b.expr_binary(BIN_SUB, b.expr_var(1), b.expr_var(2))
    b.add_constraint(b.expr_binary(BIN_OR, b.expr_in_range(b.expr_var(0), lo, b.expr_var(1)),
                                   b.expr_binary(BIN_EQ, b.expr_var(0), c(5))))
    b.add_dist_ex(0, [{"lo": lo, "hi": b.expr_var(1), "weight": b.expr_const(1)},
                      {"lo": c(5), "weight": b.expr_const(1)}])
    h = SolveHandle(b.finalize_bytes(), vars=[(16, False)] * 3)
    n = 1200
    five = 0
    expect = 0.0
    for i in range(n):
        rc, v, _ = h.randomize(i + 1)
        assert rc == RANDOMIZE_OK
        x, y, g = v[0], v[1], v[2]
        assert x == 5 or y - g <= x <= y
        five += x == 5
        expect += 1 / (g + 2)
    assert abs(five - expect) < 0.25 * expect, (five, expect)


def test_foreach_weights_read_rand_variables():
    """foreach (arr[i]) arr[i] dist { 0 :/ wz, [1:15] :/ wn }, wz and wn
    rand in [1:10]: each element is 0 with probability wz / (wz + wn), as
    solved in the same call."""
    b = SolveProblemBuilder()
    b.add_var(0, 32, False, 0, 2**32 - 1)   # wz
    b.add_var(1, 32, False, 0, 2**32 - 1)   # wn
    c = lambda v: b.expr_const(v, width=32)
    for v in (0, 1):
        b.add_constraint(b.expr_in_range(b.expr_var(v), c(1), c(10)))
    b.add_array(0, 32, False, 0)
    b.foreach_begin(0, 0)
    i = b.expr_loop_index(0)
    el = b.expr_elem(0, i)
    b.add_constraint(b.expr_binary(BIN_OR, b.expr_binary(BIN_EQ, el, c(0)),
                                   b.expr_in_range(el, c(1), c(15))))
    b.add_dist_elem_ex(0, i, [
        {"lo": c(0), "weight": b.expr_var(0), "is_per_value": False},
        {"lo": c(1), "hi": c(15), "weight": b.expr_var(1), "is_per_value": False}])
    b.foreach_end()
    h = SolveHandle(b.finalize_bytes(), vars=[(32, False), (32, False)],
                    arrays=[(32, False)])
    zeros = 0
    expect = 0.0
    for s in range(300):
        rc, v, el = h.randomize(s + 1, arrays=[[0] * 4])
        assert rc == RANDOMIZE_OK
        assert all(x <= 15 for x in el[0])
        zeros += sum(x == 0 for x in el[0])
        expect += 4 * v[0] / (v[0] + v[1])
    assert abs(zeros - expect) < 0.12 * expect, (zeros, expect)


def test_guard_reads_a_rand_variable():
    """if (m) x dist { 0 :/ 9, [1:3] :/ 1 }, m rand bit, x in [0:3]: x == 0 in
    90% of the calls with m == 1, 25% of those with m == 0."""
    b = SolveProblemBuilder()
    b.add_var(0, 8, False, 0, 255)   # x
    b.add_var(1, 1, False, 0, 1)     # m
    b.add_constraint(b.expr_in_range(b.expr_var(0), _c(b, 0), _c(b, 3)))
    b.add_dist_ex(0, [{"lo": _c(b, 0), "weight": b.expr_const(9), "is_per_value": False},
                      {"lo": _c(b, 1), "hi": _c(b, 3), "weight": b.expr_const(1),
                       "is_per_value": False}],
                  guard=b.expr_var(1))
    h = SolveHandle(b.finalize_bytes(), vars=[(8, False), (1, False)])
    zero = Counter()
    calls = Counter()
    for i in range(1600):
        rc, v, _ = h.randomize(13 * i + 1)
        assert rc == RANDOMIZE_OK
        calls[v[1]] += 1
        zero[v[1]] += v[0] == 0
    assert 0.84 < zero[1] / calls[1] < 0.96, (zero, calls)
    assert 0.17 < zero[0] / calls[0] < 0.33, (zero, calls)


def test_read_variable_constrained_by_the_dist_variable():
    """x > y couples them; x dist { [y+1 : y+1] :/ 1, 15 :/ 1 } (y + 1 as a
    sum): y is decided first among the values that leave x a solution."""
    b = SolveProblemBuilder()
    b.add_var(0, 8, False, 0, 255)   # x
    b.add_var(1, 8, False, 0, 255)   # y
    b.add_constraint(b.expr_in_range(b.expr_var(1), _c(b, 0), _c(b, 14)))
    b.add_constraint(b.expr_in_range(b.expr_var(0), _c(b, 0), _c(b, 15)))
    b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(0), b.expr_var(1)))
    y1 = b.expr_binary(BIN_ADD, b.expr_var(1), _c(b, 1))
    b.add_dist_ex(0, [{"lo": y1, "weight": b.expr_const(1), "is_per_value": False},
                      {"lo": _c(b, 15), "weight": b.expr_const(1), "is_per_value": False}])
    h = SolveHandle(b.finalize_bytes(), vars=[(8, False), (8, False)])
    hit = 0
    for i in range(600):
        rc, v, _ = h.randomize(i + 3)
        assert rc == RANDOMIZE_OK
        assert v[0] > v[1]
        hit += v[0] in (v[1] + 1, 15)
    assert hit > 0.97 * 600     # the weighted entries, not a uniform x


def test_state_and_rand_reads_together():
    """A weight read from a STATE value times a RAND one."""
    b = SolveProblemBuilder()
    b.add_var(0, 8, False, 0, 255)   # x
    b.add_var(1, 8, False, 0, 255)   # k (STATE)
    b.add_var(2, 8, False, 0, 255)   # w (rand, 1..3)
    b.set_var_kind(1, VAR_STATE)
    b.add_constraint(b.expr_in_range(b.expr_var(2), _c(b, 1), _c(b, 3)))
    b.add_constraint(b.expr_in_range(b.expr_var(0), _c(b, 0), _c(b, 1)))
    wt = b.expr_binary(BIN_MUL, b.expr_var(1), b.expr_var(2))   # k * w
    b.add_dist_ex(0, [{"lo": _c(b, 0), "weight": wt, "is_per_value": False},
                      {"lo": _c(b, 1), "weight": b.expr_const(1), "is_per_value": False}])
    h = SolveHandle(b.finalize_bytes(), vars=[(8, False)] * 3)
    zero = Counter()
    calls = Counter()
    for i in range(1500):
        rc, v, _ = h.randomize(i + 1, state={1: 3})
        assert rc == RANDOMIZE_OK
        calls[v[2]] += 1
        zero[v[2]] += v[0] == 0
    for w in (1, 2, 3):
        p = 3 * w / (3 * w + 1)
        assert abs(zero[w] / calls[w] - p) < 0.08, (w, zero, calls)


def _refused(b):
    """The compile refuses it (DVS_COMPILE_DIST_RAND): every call fails."""
    h = SolveHandle(b.finalize_bytes(), vars=[(8, False)] * 2)
    assert h.randomize(1)[0] == RANDOMIZE_ERROR


def test_read_solved_after_is_refused():
    """solve x before y, and x's dist reads y: no order has y first."""
    _refused(_rand_items(order_y=2, order_x=1))


def test_read_in_the_same_stage_is_refused():
    _refused(_rand_items(order_y=1, order_x=1))


def test_own_variable_is_refused():
    b = SolveProblemBuilder()
    b.add_var(0, 8, False, 0, 255)
    b.add_var(1, 8, False, 0, 255)
    b.add_dist_ex(0, [{"lo": b.expr_var(0), "weight": b.expr_const(1)}])
    _refused(b)


def test_cycle_is_refused():
    """x's dist reads y and y's reads x."""
    b = SolveProblemBuilder()
    b.add_var(0, 8, False, 0, 255)
    b.add_var(1, 8, False, 0, 255)
    b.add_dist_ex(0, [{"lo": b.expr_var(1), "weight": b.expr_const(1)}])
    b.add_dist_ex(1, [{"lo": b.expr_var(0), "weight": b.expr_const(1)}])
    _refused(b)


def test_chain_of_reads():
    """z's dist reads y, y's reads x: x, then y, then z. Each follows the one
    before it 90% of the time."""
    b = SolveProblemBuilder()
    for v in range(3):
        b.add_var(v, 8, False, 0, 255)
        b.add_constraint(b.expr_in_range(b.expr_var(v), _c(b, 0), _c(b, 15)))
    for v in (1, 2):
        b.add_dist_ex(v, [{"lo": b.expr_var(v - 1), "weight": b.expr_const(9),
                           "is_per_value": False},
                          {"lo": _c(b, 0), "hi": _c(b, 15), "weight": b.expr_const(1),
                           "is_per_value": False}])
    h = SolveHandle(b.finalize_bytes(), vars=[(8, False)] * 3)
    same = Counter()
    n = 800
    for i in range(n):
        rc, v, _ = h.randomize(i + 1)
        assert rc == RANDOMIZE_OK
        same["yx"] += v[1] == v[0]
        same["zy"] += v[2] == v[1]
    for k in ("yx", "zy"):
        assert 0.85 * n < same[k] < 0.97 * n, same

