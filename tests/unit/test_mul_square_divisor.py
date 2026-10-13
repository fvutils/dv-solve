"""Products the search reasons about natively (_fire_bvbin_64 in
src/c/dvs_prop_templates.c, _pick_mul_divisor in src/c/dvs_search.c):

- a square `x * x` bounds x by the integer square roots of the product's
  bounds, and a fixed product that is not a square (mod 2^w when it may wrap)
  is a conflict;
- a fixed product `a * b == K` narrows each operand to the divisors of K, and
  the operand is drawn from them.
"""
from __future__ import annotations

import collections
import time

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_ADD, BIN_EQ, BIN_GTE, BIN_MUL
from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT


def _build(nvars, widths, constrain):
    b = SolveProblemBuilder()
    try:
        for i in range(nvars):
            b.add_var(i, widths[i], False, 0, (1 << widths[i]) - 1)
        constrain(b)
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    return buf


def _ext(b, i, frm, to):
    return b.expr_extend(b.expr_var(i), frm, to, False)


def _solve(buf, seeds, limit_ms=2000):
    ctx = SolveCtx(buf)
    out = []
    try:
        for s in seeds:
            ctx.reset()
            t = time.perf_counter()
            rc = ctx.solve(seed=s, fair_pick=True, time_limit_ms=limit_ms)
            out.append((rc, time.perf_counter() - t,
                        [ctx.get_value(i) for i in range(ctx.n_vars)] if rc == SOLVE_OK else None))
    finally:
        ctx.destroy()
    return out


def test_square_of_non_square_is_unsat():
    def c(b):
        sq = b.expr_binary(BIN_MUL, _ext(b, 0, 16, 32), _ext(b, 0, 16, 32))
        b.add_constraint(b.expr_binary(BIN_EQ, sq, b.expr_const(1000001, width=32)))
    (rc, t, _), = _solve(_build(1, [16], c), [1])
    assert rc == SOLVE_UNSAT and t < 0.1


def test_square_root_pinned():
    def c(b):
        sq = b.expr_binary(BIN_MUL, _ext(b, 0, 16, 32), _ext(b, 0, 16, 32))
        b.add_constraint(b.expr_binary(BIN_EQ, sq, b.expr_const(1234 * 1234, width=32)))
    (rc, _, v), = _solve(_build(1, [16], c), [1])
    assert rc == SOLVE_OK and v[0] == 1234


def test_wrapping_square_residue():
    """x * x at 8 bits wraps; 2 (one trailing zero) and 3 (odd part 3 mod 8)
    are not squares mod 256, 17 is (1 mod 8), and so is 0."""
    for k, sat in ((2, False), (3, False), (17, True), (0, True)):
        def c(b, k=k):
            sq = b.expr_binary(BIN_MUL, b.expr_var(0), b.expr_var(0))
            b.add_constraint(b.expr_binary(BIN_EQ, sq, b.expr_const(k, width=8)))
        (rc, _, v), = _solve(_build(1, [8], c), [1])
        if sat:
            assert rc == SOLVE_OK and (v[0] * v[0]) % 256 == k
        else:
            assert rc == SOLVE_UNSAT, k


def test_product_of_prime_without_unit_factor_is_unsat():
    """a * b == 65521 * 3 with a, b >= 4: no divisor pair fits."""
    def c(b):
        p = b.expr_binary(BIN_MUL, _ext(b, 0, 16, 32), _ext(b, 1, 16, 32))
        b.add_constraint(b.expr_binary(BIN_EQ, p, b.expr_const(65521 * 3, width=32)))
        for i in range(2):
            b.add_constraint(b.expr_binary(BIN_GTE, b.expr_var(i), b.expr_const(4, width=16)))
    (rc, t, _), = _solve(_build(2, [16, 16], c), [1])
    assert rc == SOLVE_UNSAT and t < 0.1


def test_fixed_product_drawn_uniformly_over_divisor_pairs():
    """a * b == 720720 over 16-bit fields has 220 solutions. A draw from the
    hull of the divisors and a redraw on conflict favoured some (chi^2/df ~21);
    drawing among the divisors is uniform."""
    K = 720720
    def c(b):
        p = b.expr_binary(BIN_MUL, _ext(b, 0, 16, 32), _ext(b, 1, 16, 32))
        b.add_constraint(b.expr_binary(BIN_EQ, p, b.expr_const(K, width=32)))
    sols = [(d, K // d) for d in range(1, 1 << 16) if K % d == 0 and K // d < 1 << 16]
    n = 20 * len(sols)
    seen = collections.Counter()
    for rc, _, v in _solve(_build(2, [16, 16], c), range(1, n + 1)):
        assert rc == SOLVE_OK
        seen[tuple(v[:2])] += 1
    assert set(seen) == set(sols)
    e = n / len(sols)
    chi = sum((seen[s] - e) ** 2 / e for s in sols) / (len(sols) - 1)
    assert chi < 1.5, chi


def test_square_over_one_extended_operand():
    """pyvsc states `c * c` as `ext(c) * c`: only the left operand extended.
    Both are c's value, so the multiply is a square (prop_add_bvmul_64): a
    fixed product pins c, or is a conflict. Seen as two variables, interval
    division alone could not narrow a 32-bit c and the search gave up."""
    r = 3000000007
    for k, root in ((r * r, r), (r * r + 1, None)):
        def c(b, k=k):
            sq = b.expr_binary(BIN_MUL, _ext(b, 0, 32, 64), b.expr_var(0))
            b.add_constraint(b.expr_binary(BIN_EQ, sq, b.expr_const(k, width=64)))
        (rc, t, v), = _solve(_build(1, [32], c), [1])
        if root is None:
            assert rc == SOLVE_UNSAT and t < 0.05, (rc, t)
        else:
            assert rc == SOLVE_OK and v[0] == root and t < 0.05, (rc, t, v)
