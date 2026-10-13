"""BV-SAT encodes the aggregate relations (bb_array_select, bb_sum,
bb_countones and bb_clog2 in src/c/dvs_bbsolver.c).

They were reported unsupported, so a problem using one got no BV-SAT verdict:
an UNSAT one stayed undecided, since the primary search does not prove UNSAT
for its client. Each relation has the meaning the SMT-LIB2 emitter gives it
(dvs_smt2_emit.c, _aggregate). Every case here is small enough to enumerate:
the BV-SAT verdict must match the brute-force one, and a SAT model must
satisfy the constraints.
"""
from __future__ import annotations

import itertools
import random

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_EQ, BIN_NEQ, BIN_LT, BIN_OR, UN_NOT
from dv_solve.bvsat import BVSatCtx, BVSAT_SAT, BVSAT_UNSAT


def _val(raw, w, signed):
    raw &= (1 << w) - 1
    return raw - (1 << w) if signed and raw >> (w - 1) else raw


def _check(vars_, build, holds):
    """vars_: [(width, signed)]; build(b) adds the constraints; holds(vals)
    is the brute-force meaning over the variables' values."""
    b = SolveProblemBuilder()
    try:
        for i, (w, s) in enumerate(vars_):
            if s:
                b.add_var(i, w, True, -(1 << (w - 1)), (1 << (w - 1)) - 1)
            else:
                b.add_var(i, w, False, 0, (1 << w) - 1)
        build(b)
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    domains = [range(-(1 << (w - 1)), 1 << (w - 1)) if s else range(1 << w)
               for w, s in vars_]
    want = any(holds(v) for v in itertools.product(*domains))
    bb = BVSatCtx(buf)
    try:
        rc = bb.check()
        assert rc == (BVSAT_SAT if want else BVSAT_UNSAT), (rc, want)
        if rc == BVSAT_SAT:
            got = [_val(bb.value(i), w, s) for i, (w, s) in enumerate(vars_)]
            assert holds(got), got
    finally:
        bb.destroy()


def _k(b, v, w=8):
    return b.expr_const(v, width=w)


def _pin(b, i, v, w):
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(i), _k(b, v, w)))


# ---- array select ---------------------------------------------------- #

def _select_holds(n, r, idx, el_signed, r_signed):
    def holds(v):
        res, index, els = v[r], v[idx], v[:n]
        if not 0 <= index < n:
            return False
        e = els[index]
        if r_signed and el_signed:
            return res == e
        return (res & 0xFF) == (e & 0xFF)      # both zero-extended (widths <= 8)
    return holds


@pytest.mark.parametrize("seed", range(12))
def test_select_matches_brute_force(seed):
    """`r == arr[idx]` over 3 two-bit elements, a 3-bit result and a 2-bit
    index (3 is out of range), with random pins and exclusions."""
    rng = random.Random(seed)
    vars_ = [(2, False)] * 3 + [(3, False), (2, False)]
    pins = {i: rng.randrange(4) for i in range(3) if rng.random() < 0.6}
    r_pin = rng.randrange(8) if rng.random() < 0.5 else None
    not_idx = rng.randrange(4) if rng.random() < 0.5 else None

    def build(b):
        b.add_constraint(b.expr_array_select(0, 3, b.expr_var(3), b.expr_var(4)))
        for i, v in pins.items():
            _pin(b, i, v, 2)
        if r_pin is not None:
            _pin(b, 3, r_pin, 3)
        if not_idx is not None:
            b.add_constraint(b.expr_binary(BIN_NEQ, b.expr_var(4), _k(b, not_idx, 2)))

    base = _select_holds(3, 3, 4, False, False)

    def holds(v):
        return (base(v) and all(v[i] == x for i, x in pins.items())
                and (r_pin is None or v[3] == r_pin)
                and (not_idx is None or v[4] != not_idx))
    _check(vars_, build, holds)


def test_select_unsat_when_no_element_matches():
    """arr[idx] == 3 with every element pinned to 0."""
    def build(b):
        b.add_constraint(b.expr_array_select(0, 3, _k(b, 3, 2), b.expr_var(3)))
        for i in range(3):
            _pin(b, i, 0, 2)
    _check([(2, False)] * 3 + [(2, False)], build,
           lambda v: v[3] < 3 and v[v[3]] == 3 and v[:3] == [0, 0, 0])


@pytest.mark.parametrize("r_signed", [False, True])
def test_select_signedness(r_signed):
    """Signed elements: equal to a signed result as numbers, to an unsigned
    one as zero-extended bit patterns. Elements pinned to -1, 2; the result
    pinned to 7 (unsigned: 3'b111, not -1) or -1."""
    vars_ = [(3, True), (3, True), (4, r_signed), (1, False)]
    want = -1 if r_signed else 7

    def build(b):
        b.add_constraint(b.expr_array_select(0, 2, b.expr_var(2), b.expr_var(3)))
        _pin(b, 0, -1 & 7, 3)
        _pin(b, 1, 2, 3)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(2), _k(b, want & 15, 4)))

    def holds(v):
        if v[0] != -1 or v[1] != 2 or v[2] != want:
            return False
        e = v[v[3]]
        return v[2] == e if r_signed else v[2] == (e & 7)    # e zero-extended
    _check(vars_, build, holds)


def test_select_negative_index_is_out_of_range():
    """A signed index is compared with the element's position as a number,
    so a negative one selects nothing."""
    vars_ = [(2, False)] * 2 + [(2, False), (2, True)]

    def build(b):
        b.add_constraint(b.expr_array_select(0, 2, b.expr_var(2), b.expr_var(3)))
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(3), _k(b, 0, 2)))
    _check(vars_, build, lambda v: False)


def test_select_inside_an_implication():
    """`idx == 1 -> r == arr[idx]` with arr[1] pinned and r pinned to
    something else: idx must avoid 1. A nested select is the case the
    primary engine's top-level compile does not see."""
    vars_ = [(2, False)] * 3 + [(2, False), (2, False)]

    def build(b):
        sel = b.expr_array_select(0, 3, b.expr_var(3), b.expr_var(4))
        is1 = b.expr_binary(BIN_EQ, b.expr_var(4), _k(b, 1, 2))
        b.add_constraint(b.expr_binary(BIN_OR, b.expr_unary(UN_NOT, is1), sel))
        _pin(b, 1, 2, 2)
        _pin(b, 3, 3, 2)
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(4), _k(b, 2, 2)))

    def holds(v):
        if v[1] != 2 or v[3] != 3 or not v[4] < 2:
            return False
        return v[4] != 1 or (v[4] < 3 and v[3] == v[v[4]])
    _check(vars_, build, holds)


# ---- sum, countones, clog2 ------------------------------------------- #

@pytest.mark.parametrize("seed", range(6))
def test_sum_matches_brute_force(seed):
    """`r == a + b + c` as integers: no wraparound, signed terms by value."""
    rng = random.Random(seed)
    signed = [rng.random() < 0.5 for _ in range(3)]
    vars_ = [(3, s) for s in signed] + [(4, rng.random() < 0.5)]
    r_pin = rng.randrange(16)

    def build(b):
        b.add_constraint(b.expr_sum(b.expr_var(3), [b.expr_var(i) for i in range(3)]))
        _pin(b, 3, r_pin, 4)

    def holds(v):
        return v[3] == v[0] + v[1] + v[2] and (v[3] & 15) == r_pin
    _check(vars_, build, holds)


@pytest.mark.parametrize("k", range(0, 7))
def test_countones_matches_brute_force(k):
    """countones(x) == k over a 5-bit x: 6 is impossible."""
    def build(b):
        b.add_constraint(b.expr_countones(_k(b, k, 3), b.expr_var(0)))
    _check([(5, False)], build, lambda v: bin(v[0]).count("1") == k)


@pytest.mark.parametrize("k", range(0, 7))
def test_clog2_matches_brute_force(k):
    """clog2(x) == k over a 5-bit x (0 and 1 give 0; 31 gives 5)."""
    def clog2(x):
        n = 0
        while (1 << n) < x:
            n += 1
        return n

    def build(b):
        b.add_constraint(b.expr_clog2(_k(b, k, 3), b.expr_var(0)))
    _check([(5, False)], build, lambda v: clog2(v[0]) == k)


def test_select_of_substituted_elements():
    """Elements defined by `e == expr` are substituted by their expressions
    (BV-SAT's equality substitution); the select names them by id, so it
    must read them through it, and the cycle check must see that the index
    `idx == e1 - 1` refers back to an element."""
    from dv_solve.problem import BIN_ADD, BIN_SUB
    vars_ = [(3, False)] * 3 + [(3, False), (2, False), (3, False)]

    def build(b):
        v = b.expr_var
        b.add_constraint(b.expr_binary(BIN_EQ, v(0), b.expr_binary(BIN_ADD, v(5), _k(b, 1, 3))))
        b.add_constraint(b.expr_binary(BIN_EQ, v(1), b.expr_binary(BIN_ADD, v(5), _k(b, 2, 3))))
        b.add_constraint(b.expr_binary(BIN_EQ, v(4), b.expr_binary(BIN_SUB, v(1), _k(b, 1, 3))))
        b.add_constraint(b.expr_array_select(0, 3, v(3), v(4)))
        _pin(b, 3, 4, 3)

    def holds(v):
        e0, e1, e2, r, idx, a = v
        return (e0 == (a + 1) % 8 and e1 == (a + 2) % 8 and idx == (e1 - 1) % 8
                and idx < 3 and r == 4 and r == v[idx])
    _check(vars_, build, holds)
