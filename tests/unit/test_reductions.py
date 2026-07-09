"""Unit tests for SystemVerilog array reductions exercised by Verilator:
a.sum(), a.product(), a.xor(), a.and(), a.or() (and the `with` predicate form).

Engine notes (discovered while writing this file):
  * The primary CDCL engine (SolveCtx) natively compiles `expr_sum` (an n-ary
    node) and a SINGLE binary op `r == a OP b` with plain-var operands. It does
    NOT compile a NESTED binary chain `r == (a OP b) OP c` — that raises
    CompileIncompleteError. A reduction chain must therefore either introduce an
    intermediate var per step (tested here) or run on the bitblast engine.
  * The bitblast engine (BVSatCtx) compiles arbitrary nested binary chains
    natively (bvxor/bvand/bvor/bvadd/bvmul), which is the realistic path for the
    QF_UFBV that Verilator emits. It does NOT compile the high-level expr_sum /
    countones nodes (see test_sysfunc.py) — so `.sum()` reductions belong on
    CDCL and bitwise reductions belong on bitblast.

Maps to Verilator tests: t_constraint_array_sum_with,
t_constraint_dyn_array_reduction, t_constraint_array_reduction_inherit.
"""
from __future__ import annotations

import functools

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import (
    BIN_EQ, BIN_ADD, BIN_MUL, BIN_BXOR, BIN_BAND, BIN_BOR,
)
from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT, CompileUnsatError, CompileIncompleteError
from dv_solve.bvsat import BVSatCtx, BVSAT_SAT


# ------------------------------------------------------------------ #
# Engine helpers                                                      #
# ------------------------------------------------------------------ #

def _cdcl(build, seed=1):
    b = SolveProblemBuilder()
    vids = build(b)
    buf, _sz = b.finalize()
    try:
        try:
            ctx = SolveCtx(buf)
        except CompileUnsatError:
            return SOLVE_UNSAT, {}
        rc = ctx.solve(seed=seed)
        vals = {v: ctx.get_value(v) for v in vids} if rc == SOLVE_OK else {}
        ctx.destroy()
        return rc, vals
    finally:
        b.destroy()


def _bitblast(build, seed=1):
    b = SolveProblemBuilder()
    vids = build(b)
    buf, _sz = b.finalize()
    ctx = BVSatCtx(buf)
    rc = ctx.check(seed=seed)
    vals = {v: ctx.value(v) for v in vids} if rc == BVSAT_SAT else {}
    ctx.destroy()
    return (rc == BVSAT_SAT), vals


# ------------------------------------------------------------------ #
# sum() reduction — CDCL, n-ary expr_sum                             #
# ------------------------------------------------------------------ #

def test_sum_reduction_backward():
    """a.sum() == target, elements free: model must sum to target."""
    def build(b):
        b.add_var(0, 32, True, 100, 100)           # result pinned
        for i in range(1, 5):
            b.add_var(i, 32, True, 0, 50)
        b.add_constraint(b.expr_sum(b.expr_var(0), [b.expr_var(i) for i in range(1, 5)]))
        return list(range(5))

    rc, v = _cdcl(build)
    assert rc == SOLVE_OK
    assert sum(v[i] for i in range(1, 5)) == 100


def test_sum_reduction_forward():
    """Elements pinned -> result is their sum."""
    elems = [7, 11, 13, 19]
    def build(b):
        b.add_var(0, 32, True, 0, 1000)            # result free
        for i, e in enumerate(elems, start=1):
            b.add_var(i, 32, True, e, e)
        b.add_constraint(b.expr_sum(b.expr_var(0), [b.expr_var(i) for i in range(1, 5)]))
        return [0]

    rc, v = _cdcl(build)
    assert rc == SOLVE_OK
    assert v[0] == sum(elems)


def test_sum_with_predicate_count():
    """`a.sum() with (item > k)` counts elements > k. Runs on the bitblast
    engine: each guard g_i == (a[i] > k) is a comparison-as-value (not compiled
    on CDCL) and the count is an add-chain of guards (bitblast has no expr_sum).
    This split — comparison-values want bitblast, expr_sum wants CDCL — is why
    the predicate form does not fit a single engine on the builder API."""
    from dv_solve.problem import BIN_GT
    k = 10
    def build(b):
        b.add_var(0, 8, False, 2, 2)               # count pinned: exactly two > k
        for i in range(1, 5):
            b.add_var(i, 8, False, 0, 40)
        gids = []
        for gi, i in zip(range(5, 9), range(1, 5)):
            b.add_var(gi, 8, False, 0, 1)
            b.add_constraint(
                b.expr_binary(BIN_EQ, b.expr_var(gi),
                              b.expr_binary(BIN_GT, b.expr_var(i), b.expr_const(k))))
            gids.append(gi)
        ch = b.expr_var(gids[0])
        for gi in gids[1:]:
            ch = b.expr_binary(BIN_ADD, ch, b.expr_var(gi))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), ch))
        return list(range(1, 5))

    sat, v = _bitblast(build)
    assert sat
    gt = sum(1 for i in range(1, 5) if v[i] > k)
    assert gt == 2, "expected exactly 2 elems > %d, got %d (%s)" % (
        k, gt, [v[i] for i in range(1, 5)])


# ------------------------------------------------------------------ #
# product() — single mul on CDCL; chain via intermediate var         #
# ------------------------------------------------------------------ #

def test_product_single():
    def build(b):
        b.add_var(0, 16, False, 12, 12)
        b.add_var(1, 8, False, 1, 12)
        b.add_var(2, 8, False, 1, 12)
        b.add_constraint(
            b.expr_binary(BIN_EQ, b.expr_var(0),
                          b.expr_binary(BIN_MUL, b.expr_var(1), b.expr_var(2)))
        )
        return [0, 1, 2]

    rc, v = _cdcl(build)
    assert rc == SOLVE_OK
    assert v[1] * v[2] == 12


def test_xor_reduction_via_intermediates_cdcl():
    """a.xor() over 3 elements on CDCL, lowered with an intermediate var
    (the chain form the frontend must emit for the CDCL engine)."""
    def build(b):
        b.add_var(0, 8, False, 0x3C, 0x3C)         # result
        for i in (1, 2, 3):
            b.add_var(i, 8, False, 0, 255)
        b.add_var(4, 8, False, 0, 255)             # intermediate t
        b.add_constraint(
            b.expr_binary(BIN_EQ, b.expr_var(4),
                          b.expr_binary(BIN_BXOR, b.expr_var(1), b.expr_var(2))))
        b.add_constraint(
            b.expr_binary(BIN_EQ, b.expr_var(0),
                          b.expr_binary(BIN_BXOR, b.expr_var(4), b.expr_var(3))))
        return [0, 1, 2, 3]

    rc, v = _cdcl(build)
    assert rc == SOLVE_OK
    assert v[1] ^ v[2] ^ v[3] == 0x3C


def test_nested_chain_not_native_on_cdcl():
    """Documents the CDCL limitation: a nested binary chain equated to a result
    var is not compiled natively (raises CompileIncompleteError). If this ever
    starts compiling, convert to a positive assertion and drop the intermediate
    workaround in test_xor_reduction_via_intermediates_cdcl."""
    def build(b):
        b.add_var(0, 8, False, 0x3C, 0x3C)
        for i in (1, 2, 3):
            b.add_var(i, 8, False, 0, 255)
        ch = b.expr_binary(BIN_BXOR, b.expr_var(1), b.expr_var(2))
        ch = b.expr_binary(BIN_BXOR, ch, b.expr_var(3))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), ch))
        return [0]

    b = SolveProblemBuilder()
    build(b)
    buf, _sz = b.finalize()
    try:
        with pytest.raises(CompileIncompleteError):
            SolveCtx(buf)
    finally:
        b.destroy()


# ------------------------------------------------------------------ #
# Bitwise reductions — bitblast, native nested chains                #
# ------------------------------------------------------------------ #

_REDUCERS = [
    ("xor", BIN_BXOR, lambda xs: functools.reduce(lambda a, b: a ^ b, xs)),
    ("and", BIN_BAND, lambda xs: functools.reduce(lambda a, b: a & b, xs)),
    ("or",  BIN_BOR,  lambda xs: functools.reduce(lambda a, b: a | b, xs)),
    ("add", BIN_ADD,  lambda xs: functools.reduce(lambda a, b: a + b, xs)),
]


@pytest.mark.parametrize("name,op,ref", _REDUCERS)
def test_bitwise_reduction_bitblast(name, op, ref):
    """N-element reduction as a native nested chain on the bitblast engine;
    the model must satisfy result == reduce(op, elements) (mod 2**width)."""
    width = 8
    n = 4
    target = 0x3C
    def build(b):
        b.add_var(0, width, False, target, target)
        for i in range(1, n + 1):
            b.add_var(i, width, False, 0, (1 << width) - 1)
        ch = b.expr_var(1)
        for i in range(2, n + 1):
            ch = b.expr_binary(op, ch, b.expr_var(i))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), ch))
        return list(range(n + 1))

    sat, v = _bitblast(build)
    assert sat, "%s reduction should be SAT" % name
    elems = [v[i] for i in range(1, n + 1)]
    assert ref(elems) & ((1 << width) - 1) == target, (
        "%s(%s) = %#x != %#x" % (name, elems, ref(elems) & 0xFF, target))
