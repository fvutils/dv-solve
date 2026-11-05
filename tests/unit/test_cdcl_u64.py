"""Unsigned 64-bit arithmetic on CDCL (docs/signed_unsigned_ops_design.md,
step 4b): division, remainder and logical shift, the modular add,
subtract, multiply and left shift, and the bitwise and, or and xor.

CDCL holds a domain as an int64 interval, so an unsigned 64-bit value >= 2^63
is a negative pattern there. The 64-bit div/mod/lshr propagators read such
operands as uint64, and compile takes the three operators for any unsigned
64-bit operands (the result never exceeds the left operand). Each case runs on
a CDCL-pinned context: it must compile whole, and every model must match the
Python reference and pass validate_model.
"""
from __future__ import annotations

import random

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT
from dv_solve.problem import (BIN_ADD, BIN_BAND, BIN_BOR, BIN_BXOR, BIN_DIV, BIN_EQ,
                              BIN_GTE, BIN_LSHIFT, BIN_LT, BIN_LTE, BIN_MOD, BIN_MUL,
                              BIN_NEQ, BIN_RSHIFT, BIN_SUB)

M = (1 << 64) - 1
TOP = 1 << 63

OPS = {
    BIN_DIV: lambda a, b: a // b,
    BIN_MOD: lambda a, b: a % b,
    BIN_RSHIFT: lambda a, b: a >> b if b < 64 else 0,
}

MODULAR = {
    BIN_ADD: lambda a, b: (a + b) & M,
    BIN_SUB: lambda a, b: (a - b) & M,
    BIN_MUL: lambda a, b: (a * b) & M,
    BIN_LSHIFT: lambda a, b: (a << b) & M if b < 64 else 0,
}

BITWISE = {
    BIN_BAND: lambda a, b: a & b,
    BIN_BOR: lambda a, b: a | b,
    BIN_BXOR: lambda a, b: a ^ b,
}


def _k(b, v):
    return b.expr_const_wide(v & M, 64)


def _ctx(build):
    """A CDCL context over u64 vars a (0), b (1), r (2) with `build`'s constraints."""
    bld = SolveProblemBuilder()
    try:
        for i in range(3):
            bld.add_var(i, 64, False, 0, -1)
        build(bld, *(bld.expr_var(i) for i in range(3)))
        buf, _sz = bld.finalize()
    finally:
        bld.destroy()
    return SolveCtx(buf, engine="cdcl")


def _values(ctx, seeds):
    out = []
    for s in seeds:
        rc = ctx.solve(seed=s)
        if rc != SOLVE_OK:
            out.append(rc)
            continue
        assert ctx.validate_model() == 0
        out.append(tuple(ctx.get_value(i) & M for i in range(3)))
    return out


def _operands():
    rng = random.Random(64)
    edge = [0, 1, 2, 3, TOP - 1, TOP, TOP + 1, M - 1, M]
    pairs = [(a, b) for a in edge for b in edge]
    pairs += [(rng.getrandbits(64), rng.getrandbits(rng.choice([3, 20, 63, 64]))) for _ in range(60)]
    return pairs


@pytest.mark.parametrize("op", list(OPS), ids=["div", "mod", "rshift"])
def test_pinned(op):
    """r == a op b with a and b pinned: r is SV's value."""
    for a, b in _operands():
        if op == BIN_RSHIFT:
            b = b % 70
        elif b == 0:
            continue
        ctx = _ctx(lambda bl, x, y, r: (
            bl.add_constraint(bl.expr_binary(BIN_EQ, x, _k(bl, a))),
            bl.add_constraint(bl.expr_binary(BIN_EQ, y, _k(bl, b))),
            bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(op, x, y)))))
        try:
            (got,) = _values(ctx, [1])
            assert got == (a, b, OPS[op](a, b)), (hex(a), hex(b))
        finally:
            ctx.destroy()


@pytest.mark.parametrize("op,k,want", [
    (BIN_DIV, 3, 1 << 62),                  # a in [3 * 2^62, 3 * 2^62 + 2], above 2^63
    (BIN_DIV, 2, TOP - 1),
    (BIN_MOD, 1000, 999),
    (BIN_MOD, TOP + 5, 4),                  # a == 4 or a == 2^63 + 9
    (BIN_RSHIFT, 1, (1 << 62) + 5),
    (BIN_RSHIFT, 60, 15),
])
def test_dividend_found(op, k, want):
    """a op k == want with a free: every model is right, and a reaches the upper half."""
    ctx = _ctx(lambda bl, x, y, r: (
        bl.add_constraint(bl.expr_binary(BIN_EQ, y, _k(bl, k))),
        bl.add_constraint(bl.expr_binary(BIN_EQ, r, _k(bl, want))),
        bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(op, x, y)))))
    try:
        got = _values(ctx, range(1, 21))
        assert all(isinstance(g, tuple) for g in got), got
        assert all(OPS[op](a, k) == want for a, _b, _r in got)
        assert any(a >= TOP for a, _b, _r in got)
    finally:
        ctx.destroy()


def test_unsat_beyond_the_dividend():
    """a / 2 >= 2^63 has no u64 solution: the quotient is at most 2^63 - 1."""
    ctx = _ctx(lambda bl, x, y, r: (
        bl.add_constraint(bl.expr_binary(BIN_EQ, y, _k(bl, 2))),
        bl.add_constraint(bl.expr_binary(BIN_GTE, r, _k(bl, TOP))),
        bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(BIN_DIV, x, y)))))
    try:
        assert ctx.solve(seed=1) == SOLVE_UNSAT
    finally:
        ctx.destroy()


def test_variable_divisor():
    """a / b == 3 with b >= 2^62 and b != 0: a = 3b + (0..b-1) forces b < 2^63 / 1.5."""
    ctx = _ctx(lambda bl, x, y, r: (
        bl.add_constraint(bl.expr_binary(BIN_GTE, y, _k(bl, 1 << 62))),
        bl.add_constraint(bl.expr_binary(BIN_NEQ, y, _k(bl, 0))),
        bl.add_constraint(bl.expr_binary(BIN_LTE, r, _k(bl, 3))),
        bl.add_constraint(bl.expr_binary(BIN_GTE, r, _k(bl, 3))),
        bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(BIN_DIV, x, y)))))
    try:
        got = _values(ctx, range(1, 11))
        assert all(isinstance(g, tuple) and g[0] // g[1] == 3 for g in got), got
    finally:
        ctx.destroy()


@pytest.mark.parametrize("op", list(MODULAR), ids=["add", "sub", "mul", "lshift"])
def test_modular_pinned(op):
    """r == a op b with a and b pinned: r wraps modulo 2^64."""
    for a, b in _operands()[:100]:
        if op == BIN_LSHIFT:
            b = b % 70
        ctx = _ctx(lambda bl, x, y, r: (
            bl.add_constraint(bl.expr_binary(BIN_EQ, x, _k(bl, a))),
            bl.add_constraint(bl.expr_binary(BIN_EQ, y, _k(bl, b))),
            bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(op, x, y)))))
        try:
            (got,) = _values(ctx, [1])
            assert got == (a, b, MODULAR[op](a, b)), (hex(a), hex(b))
        finally:
            ctx.destroy()


@pytest.mark.parametrize("op", list(MODULAR), ids=["add", "sub", "mul", "lshift"])
def test_modular_backward(op):
    """a op k == c with a free and k, c fixed: a is found (for a multiply or a
    shift, from the inverse of k's odd part modulo 2^64, not by search)."""
    rng = random.Random(op)
    for _ in range(15):
        a0 = rng.getrandbits(64)
        k = rng.randrange(1, 64) if op == BIN_LSHIFT else rng.getrandbits(64) | 1
        if op == BIN_MUL and rng.random() < 0.5:
            k <<= rng.randrange(1, 20)          # even: 2^t solutions
            k &= M
        want = MODULAR[op](a0, k)
        ctx = _ctx(lambda bl, x, y, r: (
            bl.add_constraint(bl.expr_binary(BIN_EQ, y, _k(bl, k))),
            bl.add_constraint(bl.expr_binary(BIN_EQ, r, _k(bl, want))),
            bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(op, x, y)))))
        try:
            got = _values(ctx, range(1, 4))
            assert all(isinstance(g, tuple) for g in got), (hex(a0), hex(k), got)
            assert all(MODULAR[op](a, k) == want for a, _b, _r in got)
        finally:
            ctx.destroy()


def test_mul_no_inverse_unsat():
    """a * 2^t == c with c's low t bits not all 0 has no solution."""
    ctx = _ctx(lambda bl, x, y, r: (
        bl.add_constraint(bl.expr_binary(BIN_EQ, y, _k(bl, 12))),
        bl.add_constraint(bl.expr_binary(BIN_EQ, r, _k(bl, TOP + 2))),
        bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(BIN_MUL, x, y)))))
    try:
        assert ctx.solve(seed=1) == SOLVE_UNSAT
    finally:
        ctx.destroy()


def test_add_overflow_check():
    """a + b <u a (an unsigned overflow test) holds only when the sum wraps."""
    ctx = _ctx(lambda bl, x, y, r: (
        bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(BIN_ADD, x, y))),
        bl.add_constraint(bl.expr_binary(BIN_LT, r, x))))
    try:
        for a, b, r in _values(ctx, range(1, 11)):
            assert r == (a + b) & M and r < a and a + b > M
    finally:
        ctx.destroy()


@pytest.mark.parametrize("op", list(BITWISE), ids=["and", "or", "xor"])
def test_bitwise_pinned(op):
    """r == a op b with a and b pinned, patterns >= 2^63 included."""
    for a, b in _operands()[:100]:
        ctx = _ctx(lambda bl, x, y, r: (
            bl.add_constraint(bl.expr_binary(BIN_EQ, x, _k(bl, a))),
            bl.add_constraint(bl.expr_binary(BIN_EQ, y, _k(bl, b))),
            bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(op, x, y)))))
        try:
            (got,) = _values(ctx, [1])
            assert got == (a, b, BITWISE[op](a, b)), (hex(a), hex(b))
        finally:
            ctx.destroy()


@pytest.mark.parametrize("op", list(BITWISE), ids=["and", "or", "xor"])
def test_bitwise_backward(op):
    """a op k == c with a free: every model is right, and an `a` above 2^63
    is found when one is needed."""
    rng = random.Random(op)
    for _ in range(15):
        a0 = rng.getrandbits(64) | TOP
        k = rng.getrandbits(64)
        want = BITWISE[op](a0, k)
        ctx = _ctx(lambda bl, x, y, r: (
            bl.add_constraint(bl.expr_binary(BIN_EQ, y, _k(bl, k))),
            bl.add_constraint(bl.expr_binary(BIN_EQ, r, _k(bl, want))),
            bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(op, x, y)))))
        try:
            got = _values(ctx, range(1, 4))
            assert all(isinstance(g, tuple) for g in got), (hex(a0), hex(k), got)
            assert all(BITWISE[op](a, k) == want for a, _b, _r in got)
        finally:
            ctx.destroy()


@pytest.mark.parametrize("op,k,want", [
    (BIN_BAND, M, TOP),          # a & ~0 == 2^63: a is 2^63
    (BIN_BOR, 1, 1),             # a | 1 == 1 needs a <= 1: not above 2^63
    (BIN_BXOR, TOP, 5),          # a ^ 2^63 == 5: a = 2^63 + 5
])
def test_bitwise_exact(op, k, want):
    ctx = _ctx(lambda bl, x, y, r: (
        bl.add_constraint(bl.expr_binary(BIN_EQ, y, _k(bl, k))),
        bl.add_constraint(bl.expr_binary(BIN_EQ, r, _k(bl, want))),
        bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(op, x, y)))))
    try:
        got = _values(ctx, range(1, 6))
        assert all(isinstance(g, tuple) and BITWISE[op](g[0], k) == want for g in got), got
    finally:
        ctx.destroy()


def test_bitwise_unsat():
    """a & b with a pinned to 2^63 + 1 can't hold bit 1."""
    ctx = _ctx(lambda bl, x, y, r: (
        bl.add_constraint(bl.expr_binary(BIN_EQ, x, _k(bl, TOP + 1))),
        bl.add_constraint(bl.expr_binary(BIN_EQ, r, _k(bl, 2))),
        bl.add_constraint(bl.expr_binary(BIN_EQ, r, bl.expr_binary(BIN_BAND, x, y)))))
    try:
        assert ctx.solve(seed=1) == SOLVE_UNSAT
    finally:
        ctx.destroy()


# ---- {hi, lo} at 64 bits, and comparing a `!` (the protocol-door declines) ----

def _cat_ctx(lo_w, build, engine="cdcl"):
    """u64 r (0), hi (1) of 64 - lo_w bits, lo (2) of lo_w bits; r == {hi, lo}."""
    bld = SolveProblemBuilder()
    try:
        bld.add_var(0, 64, False, 0, -1)
        bld.add_var(1, 64 - lo_w, False, 0, (1 << (64 - lo_w)) - 1)
        bld.add_var(2, lo_w, False, 0, (1 << lo_w) - 1)
        r, hi, lo = (bld.expr_var(i) for i in range(3))
        bld.add_constraint(bld.expr_binary(BIN_EQ, r, bld.expr_concat(hi, lo, lo_w)))
        build(bld, r, hi, lo)
        buf, _sz = bld.finalize()
    finally:
        bld.destroy()
    return SolveCtx(buf, engine=engine)


def _cat_models(ctx, lo_w, seeds=range(1, 21)):
    out = []
    for s in seeds:
        assert ctx.solve(seed=s) == SOLVE_OK
        assert ctx.validate_model() == 0
        r, hi, lo = (ctx.get_value(i) & M for i in range(3))
        assert r == (hi << lo_w) | lo
        out.append((r, hi, lo))
    return out


@pytest.mark.parametrize("lo_w", [1, 17, 32, 63])
def test_concat64_forward_backward(lo_w):
    """r above 2^63 found from its parts, and the parts from a fixed r."""
    with _cat_ctx(lo_w, lambda b, r, hi, lo: b.add_constraint(
            b.expr_binary(BIN_GTE, r, _k(b, TOP + 5)))) as ctx:
        assert all(r >= TOP + 5 for r, _, _ in _cat_models(ctx, lo_w))
    want = M - 12345
    with _cat_ctx(lo_w, lambda b, r, hi, lo: b.add_constraint(
            b.expr_binary(BIN_EQ, r, _k(b, want)))) as ctx:
        assert set(_cat_models(ctx, lo_w)) == {
            (want, want >> lo_w, want & ((1 << lo_w) - 1))}


def test_concat64_unsat():
    """r < 2^63 with hi's top bit set."""
    lo_w = 20
    with _cat_ctx(lo_w, lambda b, r, hi, lo: (
            b.add_constraint(b.expr_binary(BIN_LT, r, _k(b, TOP))),
            b.add_constraint(b.expr_binary(BIN_GTE, hi, b.expr_const_wide(1 << 43, 44))))) as ctx:
        assert ctx.solve(seed=1) == SOLVE_UNSAT


def test_concat64_pin():
    """A pin on r above 2^63 sets both parts (the protocol door pins)."""
    lo_w = 40
    with _cat_ctx(lo_w, lambda b, r, hi, lo: None, engine="auto") as ctx:
        assert ctx.engine == "cdcl"
        v = TOP + (77 << lo_w) + 99
        assert ctx.pin(0, v)
        assert ctx.solve(seed=3) == SOLVE_OK
        assert [ctx.get_value(i) & M for i in range(3)] == [v, (TOP >> lo_w) + 77, 99]


@pytest.mark.parametrize("op,rhs,sat", [
    (BIN_NEQ, "true", True),     # (xor (not true) true)
    (BIN_EQ, "true", False),
    (BIN_NEQ, "x>1", True),      # (xor (not true) (> x 1)): x > 1
    (BIN_NEQ, "ite", True),      # (xor (not true) (ite true true true))
    (BIN_EQ, "ite", False),
])
def test_lnot_compare(op, rhs, sat):
    from dv_solve.problem import BIN_GT, UN_NOT
    bld = SolveProblemBuilder()
    try:
        bld.add_var(0, 8, False, 0, 255)
        one = bld.expr_const(1)
        lhs = bld.expr_unary(UN_NOT, one)
        if rhs == "true":
            r = one
        elif rhs == "ite":
            t = bld.expr_const(1, width=1)
            r = bld.expr_ite(t, t, t)
        else:
            r = bld.expr_binary(BIN_GT, bld.expr_var(0), bld.expr_const(1))
        bld.add_constraint(bld.expr_binary(op, lhs, r))
        buf, _sz = bld.finalize()
    finally:
        bld.destroy()
    with SolveCtx(buf, engine="cdcl") as ctx:
        rc = ctx.solve(seed=1)
        assert rc == (SOLVE_OK if sat else SOLVE_UNSAT)
        if sat:
            assert ctx.validate_model() == 0
            if rhs == "x>1":
                assert ctx.get_value(0) > 1
