"""Expressions over 64 bits whose variables are 64 bits or less.

The CDCL engine compiles nothing over 64 bits. A handle narrows such
expressions where an equal one of 64 bits or less exists (dvs_narrow.c):
zero/sign extensions, concatenations, slices, casts, bitwise operators,
shifts by a constant, and + - * whose result fits are taken apart into
slices of at most 64 bits, and a comparison is done at 64 bits or less or
slice by slice. What it cannot narrow is solved on bit-blast, with the STATE
values pinned, unless soft constraints, a dist or randc keep the problem on
CDCL: then dvs_problem_validate() refuses it (UNSUPPORTED_WIDTH) and
dvs_randomize() returns DVS_RANDOMIZE_ERROR, the two agreeing.

The shapes Verilator's --solver-dvs wrote, which validate passed and
randomize refused: a wide value read as a concatenation of 32-bit words in a
problem with a soft constraint; any operator over 64 bits in a problem with
arrays (whose expansions have STATE variables: a local array's size, an
element-local problem's index).
"""
from __future__ import annotations

import random

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.handle import RANDOMIZE_ERROR, RANDOMIZE_OK, RANDOMIZE_UNSAT, SolveHandle
from dv_solve.problem import (BIN_ADD, BIN_AND, BIN_ASHR, BIN_BAND, BIN_BOR, BIN_BXOR,
                              BIN_DIV, BIN_EQ, BIN_GT, BIN_GTE, BIN_LSHIFT, BIN_LT,
                              BIN_LTE, BIN_MUL, BIN_NEQ, BIN_OR, BIN_RSHIFT, BIN_SUB,
                              UN_INVERT, UN_NEG, UN_NOT, VAR_RAND, VAR_RANDC,
                              VAR_STATE)

_FULL = (1 << 63) - 1   # the declared range of a variable of 64 bits or more


def _mask(w):
    return (1 << w) - 1


def _explicit():
    b = SolveProblemBuilder()
    b.set_explicit()
    return b


def _zext(b, e, frm, to):
    return b.expr_extend(e, frm, to, False)


def _c(b, v, w):
    return b.expr_const_wide(v, w) if w > 64 else b.expr_const(v, False, w)


# ---------------------------------------------------------------------------
# words.c: a 96-bit value read as three 32-bit words, with a soft constraint
# ---------------------------------------------------------------------------

def _words(soft=True, op=BIN_GT, k=1 << 80):
    b = _explicit()
    for i in range(3):
        b.add_var(i, 32, False, 0, _mask(32))
    lo = b.expr_concat(b.expr_var(1), b.expr_var(0), 32)
    whole = b.expr_concat(b.expr_var(2), lo, 64)
    b.add_constraint(b.expr_binary(op, whole, _c(b, k, 96)))
    if soft:
        b.add_soft_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), _c(b, 5, 32)), 3)
    return b


@pytest.mark.parametrize("op", [BIN_GT, BIN_GTE, BIN_LT, BIN_LTE, BIN_EQ, BIN_NEQ])
def test_words_concat_with_soft(op):
    """{w2, w1, w0} op 96'h1_0000_0000_0000_0000_0000 with soft w0 == 5: on
    CDCL, the comparison slice by slice."""
    k = (1 << 80) | 5
    b = _words(op=op, k=k)
    ok, why = b.validate()
    assert ok, why
    h = SolveHandle(b.finalize_bytes(), vars=[(32, False)] * 3)
    cmp = {BIN_GT: lambda x: x > k, BIN_GTE: lambda x: x >= k, BIN_LT: lambda x: x < k,
           BIN_LTE: lambda x: x <= k, BIN_EQ: lambda x: x == k, BIN_NEQ: lambda x: x != k}[op]
    for seed in range(1, 30):
        rc, v, _ = h.randomize(seed * 7919)
        assert rc == RANDOMIZE_OK
        whole = v[2] << 64 | v[1] << 32 | v[0]
        assert cmp(whole)
        assert v[0] == 5   # the soft constraint holds: it can


def test_words_concat_randc():
    """The same comparison with a randc word: each value once per cycle."""
    b = _explicit()
    b.add_var(0, 3, False, 0, 7)
    b.set_var_kind(0, VAR_RANDC)
    b.add_var(1, 32, False, 0, _mask(32))
    b.add_var(2, 32, False, 0, _mask(32))
    whole = b.expr_concat(b.expr_var(2), b.expr_concat(b.expr_var(1), _zext(b, b.expr_var(0), 3, 32), 32), 64)
    b.add_constraint(b.expr_binary(BIN_GTE, whole, _c(b, (1 << 95) + 3, 96)))
    ok, why = b.validate()
    assert ok, why
    h = SolveHandle(b.finalize_bytes(), vars=[(3, False), (32, False), (32, False)])
    seen = []
    for seed in range(8):
        rc, v, _ = h.randomize(seed + 1)
        assert rc == RANDOMIZE_OK
        assert (v[2] << 64 | v[1] << 32 | v[0]) >= (1 << 95) + 3
        seen.append(v[0])
    assert sorted(seen) == list(range(8))


# ---------------------------------------------------------------------------
# arr.c / arr2.c / arr3.c: arrays and an operator over 64 bits
# ---------------------------------------------------------------------------

def _arr2(mode, wide_var=False, wide_arr=False):
    """array q (8-bit, 3 elements) + x (8-bit); one 65-bit constraint on x:
    0: zext(x) != 5   1: zext(x) + 1 != 5   2: (zext(x) + 1)[7:0] != 5
    3: zext(x) | 1 != 5   4: as 1 without the array."""
    b = _explicit()
    if mode != 4:
        b.add_array(0, 70 if wide_arr else 8, False, VAR_RAND)
    b.add_var(0, 8, False, 0, 255)
    if wide_var:
        b.add_var(1, 70, False, 0, _FULL)
    e = _zext(b, b.expr_var(0), 8, 65)
    if mode in (1, 2, 4):
        e = b.expr_binary(BIN_ADD, e, _c(b, 1, 65))
    if mode == 3:
        e = b.expr_binary(BIN_BOR, e, _c(b, 1, 65))
    if mode == 2:
        c = b.expr_binary(BIN_NEQ, b.expr_extract(e, 7, 0), _c(b, 5, 8))
    else:
        c = b.expr_binary(BIN_NEQ, e, _c(b, 5, 65))
    b.add_constraint(c)
    vars_ = [(8, False)] + ([(70, False)] if wide_var else [])
    arrays = [] if mode == 4 else [(70 if wide_arr else 8, False)]
    return b, vars_, arrays


_ARR2_OK = {0: lambda x: x != 5, 1: lambda x: x + 1 != 5, 2: lambda x: (x + 1) & 0xFF != 5,
            3: lambda x: x | 1 != 5, 4: lambda x: x + 1 != 5}


@pytest.mark.parametrize("wide_var,wide_arr", [(False, False), (True, False), (False, True)])
@pytest.mark.parametrize("mode", range(5))
def test_arrays_with_wide_operator(mode, wide_var, wide_arr):
    """arr2.c and arr3.c: each shape solves, and validate passes it."""
    b, vars_, arrays = _arr2(mode, wide_var, wide_arr)
    ok, why = b.validate()
    assert ok, why
    h = SolveHandle(b.finalize_bytes(), vars=vars_, arrays=arrays)
    xs = set()
    for seed in range(1, 40):
        rc, v, el = h.randomize(seed, arrays=[[0, 0, 0]] if arrays else None)
        assert rc == RANDOMIZE_OK
        assert _ARR2_OK[mode](v[0])
        xs.add(v[0])
        if arrays:
            assert len(el[0]) == 3
    assert len(xs) > 10


@pytest.mark.parametrize("wide_var", [False, True])
def test_foreach_wide_index_expression(wide_var):
    """arr.c cases 1-2: foreach (q[i]) q[i] != (65'(i) + 1)[7:0], with and
    without a 70-bit variable beside it."""
    b = _explicit()
    b.add_array(0, 8, False, VAR_RAND)
    if wide_var:
        b.add_var(0, 70, False, 0, _FULL)
    else:
        b.add_var(0, 8, False, 0, 255)
    b.foreach_begin(0, 0)
    i = b.expr_loop_index(0)
    s = b.expr_binary(BIN_ADD, b.expr_extend(i, 32, 65, True), _c(b, 1, 65))
    b.add_constraint(b.expr_binary(BIN_NEQ, b.expr_elem(0, i), b.expr_extract(s, 7, 0)))
    b.foreach_end()
    ok, why = b.validate()
    assert ok, why
    h = SolveHandle(b.finalize_bytes(), vars=[(70 if wide_var else 8, False)],
                    arrays=[(8, False)])
    for seed in range(1, 30):
        rc, _, el = h.randomize(seed, arrays=[[0] * 5])
        assert rc == RANDOMIZE_OK
        assert all(q != (k + 1) & 0xFF for k, q in enumerate(el[0]))


def test_foreach_wide_elements_scalar_operator_soft():
    """An array of 8-bit elements, a soft on them, and a 96-bit comparison of
    a scalar in a foreach: narrowed in the template, so it stays on CDCL."""
    b = _explicit()
    b.add_array(0, 8, False, VAR_RAND)
    b.add_var(0, 16, False, 0, _mask(16))
    b.foreach_begin(0, 0)
    i = b.expr_loop_index(0)
    q = _zext(b, b.expr_elem(0, i), 8, 96)
    x = _zext(b, b.expr_var(0), 16, 96)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_binary(BIN_ADD, q, x), _c(b, 300, 96)))
    b.add_soft_constraint(b.expr_binary(BIN_EQ, b.expr_elem(0, i), _c(b, 7, 8)), 1)
    b.foreach_end()
    ok, why = b.validate()
    assert ok, why
    h = SolveHandle(b.finalize_bytes(), vars=[(16, False)], arrays=[(8, False)])
    for seed in range(1, 20):
        rc, v, el = h.randomize(seed, arrays=[[0] * 4])
        assert rc == RANDOMIZE_OK
        assert all(e + v[0] < 300 for e in el[0])
        if v[0] <= 292:
            assert el[0] == [7] * 4


# ---------------------------------------------------------------------------
# What cannot be narrowed: validate and randomize agree
# ---------------------------------------------------------------------------

def _unnarrowable(b, kind):
    """A 96-bit operator whose result does not fit 63 bits (a product of two
    40-bit values), or a division over 64 bits."""
    b.add_var(0, 40, False, 0, _mask(40))
    b.add_var(1, 40, False, 0, _mask(40))
    x, y = _zext(b, b.expr_var(0), 40, 96), _zext(b, b.expr_var(1), 40, 96)
    if kind == "mul":
        e = b.expr_binary(BIN_MUL, x, y)
        return b.expr_binary(BIN_GT, e, _c(b, 1 << 70, 96))
    e = b.expr_binary(BIN_DIV, x, _c(b, 3, 96))
    return b.expr_binary(BIN_EQ, e, _c(b, 12345, 96))


@pytest.mark.parametrize("kind", ["mul", "div"])
def test_unnarrowable_with_soft_is_refused(kind):
    b = _explicit()
    b.add_constraint(_unnarrowable(b, kind))
    b.add_soft_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), _c(b, 77, 40)), 1)
    ok, why = b.validate()
    assert not ok
    assert why.startswith("UNSUPPORTED_WIDTH:") and "96-bit" in why and "narrow" in why, why
    h = SolveHandle(b.finalize_bytes(), vars=[(40, False)] * 2)
    assert h.randomize(1)[0] == RANDOMIZE_ERROR


@pytest.mark.parametrize("kind", ["mul", "div"])
def test_unnarrowable_with_state_solves_on_bitblast(kind):
    """No soft, dist or randc: a STATE variable keeps the problem on CDCL,
    which leaves the constraint out; it is solved on bit-blast, the STATE
    value pinned."""
    b = _explicit()
    root = _unnarrowable(b, kind)
    b.add_var(2, 8, False, 0, 255)
    b.set_var_kind(2, VAR_STATE)
    b.add_constraint(root)
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_extract(b.expr_var(0), 7, 0), b.expr_var(2))
                     if kind == "mul" else b.expr_binary(BIN_NEQ, b.expr_var(2), _c(b, 0, 8)))
    ok, why = b.validate()
    assert ok, why
    h = SolveHandle(b.finalize_bytes(), vars=[(40, False), (40, False), (8, False)])
    for s in (200, 13, 200, 255):
        rc, v, _ = h.randomize(s, state={2: s})
        assert rc == RANDOMIZE_OK
        assert v[2] == s
        if kind == "mul":
            assert v[0] * v[1] > 1 << 70 and v[0] & 0xFF == s
        else:
            assert v[0] // 3 == 12345


def test_soft_with_wide_var_still_refused():
    """A variable over 64 bits with a soft constraint: bit-blast only, and it
    cannot honour the soft constraint (unchanged)."""
    b = _explicit()
    b.add_var(0, 70, False, 0, _FULL)
    b.add_soft_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), _c(b, 7, 70)), 1)
    ok, why = b.validate()
    assert not ok and why.startswith("UNSUPPORTED_WIDTH:"), why


# ---------------------------------------------------------------------------
# Differential: random explicit expressions against a Python evaluator
# ---------------------------------------------------------------------------

# STATE variables: (width, signed), small enough to sample their values.
_SV = [(4, False), (5, True), (6, False)]


def _sx(v, w):
    v &= _mask(w)
    return v - (1 << w) if v >> (w - 1) & 1 else v


class _Gen:
    """Random EXPLICIT expressions: every operator's operands share its type.
    Each is returned with its evaluator: env -> its pattern (unsigned)."""

    def __init__(self, b, rng):
        self.b, self.r = b, rng
        self.var_leaves = set()

    def cast(self, n, f, frm, to, sx, ds):
        b = self.b
        mf = _mask(frm)
        if to == frm:
            return b.expr_sv_cast(n, frm, to, False, ds), lambda e: f(e) & mf

        def ev(e):
            v = f(e) & mf
            if sx and v >> (frm - 1) & 1:
                v |= _mask(to) & ~mf
            return v
        return b.expr_sv_cast(n, frm, to, sx, ds), ev

    def leaf(self, W, S):
        b, r = self.b, self.r
        if r.random() < 0.6:
            v = r.randrange(len(_SV))
            wv, sv = _SV[v]
            fv = lambda e, v=v, wv=wv: e[v] & _mask(wv)
            if W < wv:
                # Narrower than the variable: a slice (a cast only widens).
                n = b.expr_extract(b.expr_var(v), W - 1, 0)
                fn = lambda e, fv=fv: fv(e) & _mask(W)
                n, fn = (b.expr_sv_cast(n, W, W, False, True), fn) if S else (n, fn)
            else:
                n, fn = self.cast(b.expr_var(v), fv, wv, W, sv, S)
            self.var_leaves.add(n)
            return n, fn
        k = r.choice([0, 1, 2, 5, 15, 31, r.randrange(1 << min(W, 12)),
                      _mask(W), _mask(W) - 1, 1 << (W - 1), (1 << (W - 1)) - 1,
                      1 << 64, (1 << 64) - 1, 1 << 63, r.randrange(1 << W)]) & _mask(W)
        if W > 64:
            n = b.expr_const_wide(k, W, S)
        else:
            n = b.expr_const(_sx(k, W) if S else (k if k < 1 << 63 else k - (1 << 64)), S, W)
        return n, lambda e, k=k: k

    def val(self, W, S, d):
        b, r = self.b, self.r
        if d <= 0:
            return self.leaf(W, S)
        m = _mask(W)
        c = r.random()
        if c < 0.15:
            return self.leaf(W, S)
        if W <= 64 and c < 0.56:
            # At 64 bits or less only what holds a wide node (a slice), or
            # moves bits: the CDCL compile's own coverage of narrow
            # arithmetic is not what this tests.
            c = r.choice([0.6, 0.75, 0.85])
        if c < 0.30:
            op = r.choice([BIN_ADD, BIN_SUB, BIN_MUL, BIN_ADD, BIN_SUB])
            (x, fx), (y, fy) = self.val(W, S, d - 1), self.val(W, S, d - 1)
            fn = {BIN_ADD: lambda e: (fx(e) + fy(e)) & m, BIN_SUB: lambda e: (fx(e) - fy(e)) & m,
                  BIN_MUL: lambda e: (fx(e) * fy(e)) & m}[op]
            return b.expr_binary(op, x, y), fn
        if c < 0.42:
            op = r.choice([BIN_BAND, BIN_BOR, BIN_BXOR])
            (x, fx), (y, fy) = self.val(W, S, d - 1), self.val(W, S, d - 1)
            fn = {BIN_BAND: lambda e: fx(e) & fy(e), BIN_BOR: lambda e: fx(e) | fy(e),
                  BIN_BXOR: lambda e: fx(e) ^ fy(e)}[op]
            return b.expr_binary(op, x, y), fn
        if c < 0.48:
            op = r.choice([UN_NEG, UN_INVERT])
            x, fx = self.val(W, S, d - 1)
            fn = (lambda e: (-fx(e)) & m) if op == UN_NEG else (lambda e: ~fx(e) & m)
            return b.expr_unary(op, x), fn
        if c < 0.56:
            # >> of a signed value is not EXPLICIT form (a writer casts it).
            op = r.choice([BIN_LSHIFT, BIN_ASHR] + ([] if S else [BIN_RSHIFT]))
            k = r.choice([0, 1, 3, 8, 31, 32, 63, 64, W - 1, W, W + 3])
            x, fx = self.val(W, S, d - 1)
            if op == BIN_LSHIFT:
                fn = lambda e: (fx(e) << k) & m
            elif op == BIN_RSHIFT or not S:
                fn = lambda e: fx(e) >> k
            else:
                fn = lambda e: (_sx(fx(e), W) >> k) & m
            return b.expr_binary(op, x, b.expr_const(k, False, 32)), fn
        if c < 0.70 and W >= 2:
            wl = r.randrange(1, W)
            (h, fh), (lo, fl) = self.val(W - wl, False, d - 1), self.val(wl, False, d - 1)
            n = b.expr_concat(h, lo, wl)
            fn = lambda e: fh(e) << wl | fl(e)
            return (b.expr_sv_cast(n, W, W, False, True), fn) if S else (n, fn)
        if c < 0.80:
            w2 = W + r.choice([1, 3, 30, 64])
            lo = r.randrange(0, w2 - W + 1)
            x, fx = self.val(w2, r.random() < 0.5, d - 1)
            n = b.expr_extract(x, lo + W - 1, lo)
            fn = lambda e: (fx(e) >> lo) & m
            return (b.expr_sv_cast(n, W, W, False, True), fn) if S else (n, fn)
        if c < 0.90:
            # An extension of something narrower (often narrow).
            wn = r.choice([1, 3, 8, 16, 32, 40, 63, 64, W - 1]) if W > 1 else 1
            wn = max(1, min(wn, W))
            sn = r.random() < 0.5
            x, fx = self.val(wn, sn, d - 1)
            if r.random() < 0.5:
                n = b.expr_extend(x, wn, W, sn)
                ev = self.cast(x, fx, wn, W, sn, sn)[1]
                return (b.expr_sv_cast(n, W, W, False, S), ev) if sn != S else (n, ev)
            return self.cast(x, fx, wn, W, sn, S)
        if W <= 64:
            return self.leaf(W, S)
        cnd, fc = self.boolean(d - 1)
        (x, fx), (y, fy) = self.val(W, S, d - 1), self.val(W, S, d - 1)
        return b.expr_ite(cnd, x, y), lambda e: fx(e) if fc(e) else fy(e)

    def cmp(self, d):
        r = self.r
        W = r.choice([65, 65, 70, 96, 100, 128, 130]) if r.random() < 0.8 else r.choice([8, 16, 32])
        S = r.random() < 0.4
        op = r.choice([BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE, BIN_GT, BIN_GTE])
        (x, fx), (y, fy) = self.val(W, S, d), self.val(W, S, d)
        if W <= 64:
            # A variable on one side: the CDCL compile takes no comparison
            # of constants alone, whatever its width.
            while True:
                x, fx = self.leaf(W, S)
                if x in self.var_leaves:
                    break
        rd = (lambda v: _sx(v, W)) if S else (lambda v: v)
        fn = {BIN_EQ: lambda a, c: a == c, BIN_NEQ: lambda a, c: a != c,
              BIN_LT: lambda a, c: a < c, BIN_LTE: lambda a, c: a <= c,
              BIN_GT: lambda a, c: a > c, BIN_GTE: lambda a, c: a >= c}[op]
        return self.b.expr_binary(op, x, y), lambda e: fn(rd(fx(e)), rd(fy(e)))

    def boolean(self, d):
        b, r = self.b, self.r
        c = r.random()
        if d <= 0 or c < 0.7:
            return self.cmp(max(d, 0))
        if c < 0.85:
            op = r.choice([BIN_AND, BIN_OR])
            (x, fx), (y, fy) = self.boolean(d - 1), self.boolean(d - 1)
            fn = (lambda e: fx(e) and fy(e)) if op == BIN_AND else (lambda e: fx(e) or fy(e))
            return b.expr_binary(op, x, y), fn
        x, fx = self.boolean(d - 1)
        return b.expr_unary(UN_NOT, x), lambda e: not fx(e)


def _fuzz_case(seed, soft):
    rng = random.Random(seed)
    b = _explicit()
    for v, (w, s) in enumerate(_SV):
        lo, hi = (-(1 << (w - 1)), (1 << (w - 1)) - 1) if s else (0, _mask(w))
        b.add_var(v, w, s, lo, hi)
        b.set_var_kind(v, VAR_STATE)
    rv = len(_SV)
    b.add_var(rv, 3, False, 0, 7)
    root, fn = _Gen(b, rng).boolean(rng.choice([1, 2, 3]))
    b.add_constraint(root)
    if soft:
        b.add_soft_constraint(b.expr_binary(BIN_EQ, b.expr_var(rv), b.expr_const(1, False, 3)), 1)
    ok, why = b.validate()
    h = SolveHandle(b.finalize_bytes(), vars=list(_SV) + [(3, False)])
    envs = [[rng.randrange(-(1 << (w - 1)), 1 << (w - 1)) if s else rng.randrange(1 << w)
             for w, s in _SV] for _ in range(12)]
    return ok, why, h, fn, envs, rv


@pytest.mark.parametrize("chunk", range(8))
def test_fuzz_soft_validate_agrees(chunk):
    """With a soft constraint the problem stays on CDCL: validate passes it
    exactly when randomize solves it, and then every verdict is the
    evaluator's (STATE values pinned, so it is OK or UNSAT)."""
    n_ok = 0
    for i in range(40):
        seed = 1000 * chunk + i
        ok, why, h, fn, envs, rv = _fuzz_case(seed, True)
        n_ok += ok
        for k, env in enumerate(envs):
            rc, v, _ = h.randomize(k + 1, state=dict(enumerate(env)))
            if not ok:
                assert rc == RANDOMIZE_ERROR, (seed, why)
                assert why.startswith("UNSUPPORTED_WIDTH:"), why
                break
            want = fn(env)
            assert rc == (RANDOMIZE_OK if want else RANDOMIZE_UNSAT), (seed, env, rc)
            if rc == RANDOMIZE_OK:
                assert v[rv] == 1
    assert n_ok >= 10   # most shapes narrow


@pytest.mark.parametrize("chunk", range(4))
def test_fuzz_state_never_errors(chunk):
    """Without soft, dist or randc validate passes every case and randomize
    solves each, narrowed on CDCL or pinned on bit-blast."""
    for i in range(30):
        seed = 50000 + 1000 * chunk + i
        ok, why, h, fn, envs, rv = _fuzz_case(seed, False)
        assert ok, why
        for k, env in enumerate(envs[:6]):
            rc, v, _ = h.randomize(k + 1, state=dict(enumerate(env)))
            assert rc == (RANDOMIZE_OK if fn(env) else RANDOMIZE_UNSAT), (seed, env, rc)
