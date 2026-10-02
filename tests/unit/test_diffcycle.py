"""Negative-cycle detection over difference relations (dvs_diffcycle.c).

``y >= x`` and ``y + n <= x + m`` with ``n > m`` have no solution. Bounds
propagation proves it only by walking x's and y's bounds toward each other,
``n - m`` per round: over a 2^40-wide range that is ~2^40 rounds. Read as a
difference graph it is a negative cycle, which the detector reports at once.

An add is an integer equation only while its operands cannot wrap. With an
unbounded 64-bit ``y`` the same constraints ARE satisfiable -- ``y + n`` wraps
past 2^64 to something small -- and the detector must not call that UNSAT.
"""
import random
import time

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx
from dv_solve.problem import BIN_ADD, BIN_GTE, BIN_LTE

SOLVE_OK, SOLVE_UNSAT = 0, 1
X, Y, N, M = range(4)
MASK64 = (1 << 64) - 1


def _cycle(n_rng, m_rng, xy_hi):
    """y >= x;  y + n <= x + m;  x, y in [0, xy_hi];  64-bit unsigned adds."""
    b = SolveProblemBuilder()
    for v, (lo, hi) in ((X, (0, xy_hi)), (Y, (0, xy_hi)), (N, n_rng), (M, m_rng)):
        b.add_var(v, 64, False, lo, hi)
    v = b.expr_var
    b.add_constraint(b.expr_binary(BIN_GTE, v(Y), v(X)))
    b.add_constraint(b.expr_binary(BIN_LTE, b.expr_binary(BIN_ADD, v(Y), v(N)),
                                   b.expr_binary(BIN_ADD, v(X), v(M))))
    buf, _ = b.finalize()
    return b, buf


def _holds(x, y, n, m):
    return y >= x and ((y + n) & MASK64) <= ((x + m) & MASK64)


def _solve(buf, seed, **kw):
    ctx = SolveCtx(buf)
    try:
        rc = ctx.solve(seed=seed, time_limit_ms=5000, **kw)
        vals = [ctx.get_value(i) & MASK64 for i in range(4)] if rc == SOLVE_OK else None
        return rc, vals
    finally:
        ctx.destroy()


@pytest.mark.parametrize("lcg", [False, True])
def test_an_infeasible_cycle_is_unsat_at_once(lcg):
    b, buf = _cycle((20, 30), (1, 10), (1 << 40) - 1)
    try:
        t = time.perf_counter()
        rc, _ = _solve(buf, 1, use_lcg=lcg)
        assert rc == SOLVE_UNSAT
        assert time.perf_counter() - t < 1.0
    finally:
        b.destroy()


def test_a_wrapping_add_is_not_an_integer_equation():
    """Unbounded y: y + n wraps, and the constraints are satisfiable."""
    b, buf = _cycle((20, 30), (1, 10), MASK64)
    try:
        for seed in range(1, 21):
            rc, vals = _solve(buf, seed)
            assert rc == SOLVE_OK, "seed %d" % seed
            assert _holds(*vals), vals
    finally:
        b.destroy()


def test_a_feasible_cycle_solves():
    b, buf = _cycle((1, 10), (5, 30), (1 << 20) - 1)
    try:
        for seed in range(1, 51):
            rc, vals = _solve(buf, seed)
            assert rc == SOLVE_OK and _holds(*vals), (seed, vals)
    finally:
        b.destroy()


def test_random_cycles_agree_with_and_without_learning():
    """The detector's conflicts are explained to clause learning: an unsound
    explanation shows up as a verdict that differs between the two modes."""
    rng = random.Random(7)
    for _ in range(60):
        nlo = rng.randint(0, 40); nhi = nlo + rng.randint(0, 20)
        mlo = rng.randint(0, 40); mhi = mlo + rng.randint(0, 20)
        hi = (1 << rng.choice([8, 20, 40])) - 1
        expect_sat = mhi >= nlo           # y = x, n = nlo, m = mhi
        b, buf = _cycle((nlo, nhi), (mlo, mhi), hi)
        try:
            for lcg in (False, True):
                rc, vals = _solve(buf, rng.randint(1, 1 << 30), use_lcg=lcg)
                assert rc == (SOLVE_OK if expect_sat else SOLVE_UNSAT), \
                    (nlo, nhi, mlo, mhi, hi, lcg, rc)
                if vals:
                    assert _holds(*vals), vals
        finally:
            b.destroy()
