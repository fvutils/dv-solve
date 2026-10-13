"""Products the search reasons about natively (_fire_bvbin_64 in
src/c/dvs_prop_templates.c, _pick_mul_divisor and _build_dec_tier in
src/c/dvs_search.c):

- a square `x * x` bounds x by the integer square roots of the product's
  bounds, and a fixed product that is not a square (mod 2^w when it may wrap)
  is a conflict;
- a fixed product `a * b == K` narrows each operand to the divisors of K, and
  the operand is drawn from them;
- the root of a square is decided before the operands of a product, and kept
  with probability proportional to the solutions it leaves.
"""
from __future__ import annotations

import collections
import time

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_ADD, BIN_EQ, BIN_GTE, BIN_LTE, BIN_MUL
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


def test_mul_sq_solves_in_default_order():
    """a * b == c * c + 12345 over 16-bit fields timed out (2 s) every seed
    with a, b decided first, and took ~60 ms a solve with the square and
    divisor rules alone. c is now decided first, which fixes the product, and
    a is drawn from its divisors: ~15 us."""
    def c(b):
        t3 = b.expr_binary(BIN_MUL, _ext(b, 0, 16, 32), _ext(b, 1, 16, 32))
        t4 = b.expr_binary(BIN_MUL, _ext(b, 2, 16, 32), _ext(b, 2, 16, 32))
        b.add_constraint(b.expr_binary(
            BIN_EQ, t3, b.expr_binary(BIN_ADD, t4, b.expr_const(12345, width=32))))
    for rc, t, v in _solve(_build(3, [16, 16, 16], c), range(1, 11)):
        assert rc == SOLVE_OK and t < 0.02, (rc, t)
        a, bb, cc = v[:3]
        assert a * bb == cc * cc + 12345


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


def test_mul_sq_as_pyvsc_states_it():
    """pyvsc's encoding of the same constraint: the product variables
    declared first, a copy v5 of the left side, and each product's operands
    given as `ext(x) * y`: only the left one extended. The multiply then got
    c's extension and c itself, two variables. Both are c's value: still a
    square (prop_add_bvmul_64)."""
    b = SolveProblemBuilder()
    try:
        for i, w in ((5, 32), (4, 32), (3, 32), (2, 16), (1, 16), (0, 16)):
            b.add_var(i, w, False, 0, (1 << w) - 1)
        v = b.expr_var
        b.add_constraint(b.expr_binary(BIN_EQ, v(3), v(5)))
        b.add_constraint(b.expr_binary(
            BIN_EQ, v(5), b.expr_binary(BIN_ADD, v(4), b.expr_const(12345, width=32))))
        b.add_constraint(b.expr_binary(
            BIN_EQ, v(4), b.expr_binary(BIN_MUL, _ext(b, 2, 16, 32), v(2))))
        b.add_constraint(b.expr_binary(
            BIN_EQ, v(3), b.expr_binary(BIN_MUL, _ext(b, 0, 16, 32), v(1))))
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    for rc, t, val in _solve(buf, range(1, 11)):
        assert rc == SOLVE_OK and t < 0.05, (rc, t)
        assert val[0] * val[1] == val[2] * val[2] + 12345


def _mul_sq8(extra=None):
    """a * b == c * c + 345 over 8-bit fields, the products at 16 bits."""
    def c(b):
        t3 = b.expr_binary(BIN_MUL, _ext(b, 0, 8, 16), _ext(b, 1, 8, 16))
        t4 = b.expr_binary(BIN_MUL, _ext(b, 2, 8, 16), _ext(b, 2, 8, 16))
        b.add_constraint(b.expr_binary(
            BIN_EQ, t3, b.expr_binary(BIN_ADD, t4, b.expr_const(345, width=16))))
        if extra:
            extra(b)
    return _build(3, [8, 8, 8], c)


def test_square_root_weighted_by_its_solutions():
    """With c decided first, each c was equally likely, but c leaves from 1
    to 12 pairs (a, b): chi^2/df 6.9 over the 180 solutions. c is now kept
    with probability (its pairs) / (the most pairs seen) (_mul_count_accept),
    and a root with no pair is redrawn rather than split around."""
    sols = {(a, k // a, c) for c in range(256) for k in [c * c + 345]
            for a in range(1, 256) if k % a == 0 and k // a < 256}
    n = 20 * len(sols)
    seen = collections.Counter()
    for rc, _, v in _solve(_mul_sq8(), range(1, n + 1)):
        assert rc == SOLVE_OK
        seen[tuple(v[:3])] += 1
    assert set(seen) == sols
    e = n / len(sols)
    chi = sum((seen[x] - e) ** 2 / e for x in sols) / (len(sols) - 1)
    assert chi < 1.4, chi


def test_square_root_weighted_as_pyvsc_states_it():
    """The same weighting in pyvsc's encoding (see
    test_mul_sq_as_pyvsc_states_it), 8-bit: v5 is merged into v3 by
    `v3 == v5`, and a merged variable's own domain stays wide, so it is
    read through its root rather than taken for something left to solve."""
    b = SolveProblemBuilder()
    try:
        for i, w in ((5, 16), (4, 16), (3, 16), (2, 8), (1, 8), (0, 8)):
            b.add_var(i, w, False, 0, (1 << w) - 1)
        v = b.expr_var
        b.add_constraint(b.expr_binary(BIN_EQ, v(3), v(5)))
        b.add_constraint(b.expr_binary(
            BIN_EQ, v(5), b.expr_binary(BIN_ADD, v(4), b.expr_const(345, width=16))))
        b.add_constraint(b.expr_binary(
            BIN_EQ, v(4), b.expr_binary(BIN_MUL, _ext(b, 2, 8, 16), v(2))))
        b.add_constraint(b.expr_binary(
            BIN_EQ, v(3), b.expr_binary(BIN_MUL, _ext(b, 0, 8, 16), v(1))))
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    sols = {(a, k // a, c) for c in range(256) for k in [c * c + 345]
            for a in range(1, 256) if k % a == 0 and k // a < 256}
    n = 20 * len(sols)
    seen = collections.Counter()
    for rc, _, val in _solve(buf, range(1, n + 1)):
        assert rc == SOLVE_OK
        seen[tuple(val[:3])] += 1
    assert set(seen) == sols
    e = n / len(sols)
    chi = sum((seen[x] - e) ** 2 / e for x in sols) / (len(sols) - 1)
    assert chi < 1.4, chi


def test_square_root_with_more_left_than_the_product():
    """`a <= b` is left over beside the product once c is decided: the count
    of divisor pairs is not the number of solutions, so the root is not
    weighted by it. The solve is still right and fast."""
    def le(b):
        b.add_constraint(b.expr_binary(BIN_LTE, b.expr_var(0), b.expr_var(1)))
    for rc, t, v in _solve(_mul_sq8(le), range(1, 201)):
        assert rc == SOLVE_OK and t < 0.02, (rc, t)
        a, bb, c = v[:3]
        assert a * bb == c * c + 345 and a <= bb

