"""A conflict nobody undoes must still be there on the next look (B104).

Propagation dequeues a propagator, fires it, and on a conflict returns. Most
conflicts empty a domain, and the next solve's scan for empty domains finds
them; but a propagator can fail with every domain still non-empty (`x > x`,
an ite whose result fits neither branch, a constant-false comparison). The
propagator was then off the queue, and with no backtrack to restore the
bounds, the next solve -- after an unsat solve with no reset, or after a pin
that was refused -- started from the contradictory state, found nothing
queued, and returned a model that broke the constraint. Found by the
soundness campaign's builder[again] and "solved anyway" checks.
"""
from __future__ import annotations

import pytest

from tests.formal.soundness.doors import _lower
from tests.formal.soundness.ir import ev

dv = pytest.importorskip("dv_solve")
from dv_solve.builder import SolveProblemBuilder            # noqa: E402
from dv_solve.ctx import (CompileUnsatError, SOLVE_OK, SOLVE_UNSAT,  # noqa: E402
                          SolveCtx)

W = 4
X, Y, Z, V = (("var", n, W) for n in "xyzv")
IDS = {"x": 0, "y": 1, "z": 2, "v": 3}


def _k(v):
    return ("const", v, W)


ITE = ("ite", W, ("bvult", "bool", _k(0), Z), X, Y)

# (constraints, a pin that the constraints refuse)
CASES = {
    "x>x":       ([("bvugt", "bool", X, X)], ("x", 5)),
    "x<x,x=0":   ([("bvult", "bool", X, X), ("=", "bool", X, _k(0))], ("x", 0)),
    "x>y,x=y":   ([("bvugt", "bool", X, Y), ("=", "bool", X, Y)], ("y", 3)),
    "ite<y+0":   ([("bvult", "bool", ITE, ("bvadd", W, Y, _k(0))),
                   ("=", "bool", X, _k(15))], ("x", 15)),
    "ite<zext":  ([("bvult", "bool", ("zero_extend", 8, ITE), ("zero_extend", 8, Y)),
                   ("=", "bool", X, _k(15))], ("x", 15)),
    "~x==-x":    ([("=", "bool", ("bvnot", W, X), ("bvneg", W, X))], ("x", 3)),
}


def _ctx(cons):
    b = SolveProblemBuilder()
    for i in range(4):
        b.add_var(i, width=W, is_signed=False, lo=0, hi=(1 << W) - 1)
    for c in cons:
        b.add_constraint(_lower(b, c, IDS))
    try:
        return SolveCtx(b.finalize()[0])
    except CompileUnsatError:
        return None


def _model_ok(ctx, cons):
    m = {n: ctx.get_value(i) & ((1 << W) - 1) for n, i in IDS.items()}
    return all(ev(c, m) for c in cons)


@pytest.mark.parametrize("name", sorted(CASES))
def test_unsat_again_without_reset(name):
    cons, _ = CASES[name]
    ctx = _ctx(cons)
    if ctx is None:
        return
    with ctx:
        for s in range(1, 5):
            rc = ctx.solve(seed=s)
            assert rc != SOLVE_OK or _model_ok(ctx, cons), (name, s)
            assert rc == SOLVE_UNSAT, (name, s, rc)


def _is_pin(c):
    return c[0] == "=" and c[2][0] == "var" and c[3][0] == "const"


@pytest.mark.parametrize("name", sorted(CASES))
def test_solve_after_refused_pin(name):
    cons, (var, val) = CASES[name]
    base = [c for c in cons if not _is_pin(c)]
    pins = [(c[2][1], c[3][1]) for c in cons if _is_pin(c)] + [(var, val)]
    ctx = _ctx(base)
    if ctx is None:
        return
    with ctx:
        cp = ctx.checkpoint()
        for n, v in pins:
            if not ctx.pin(IDS[n], v):
                break
        else:
            pytest.fail(f"{name}: every pin taken")
        judged = base + [("=", "bool", ("var", n, W), _k(v)) for n, v in pins]
        for s in range(1, 4):
            rc = ctx.solve(seed=s)
            assert rc != SOLVE_OK or _model_ok(ctx, judged), (name, s)
        ctx.restore(cp)
        # The restore undoes the refusal: the constraints alone are judged.
        rc = ctx.solve(seed=1)
        assert rc != SOLVE_OK or _model_ok(ctx, base), name


def test_propagate_only_reports_it_twice():
    ctx = _ctx(CASES["x>x"][0])
    if ctx is None:
        return
    with ctx:
        assert ctx.propagate_only() != 0
        assert ctx.propagate_only() != 0


# ---- B109: the last solve's search is not the caller's state ---------------
#
# A solve leaves its search on the trail: a model's decisions (get_value reads
# them), or those of a search that gave up. A second solve with no reset sealed
# that state as its root; after a timed-out solve, the dead search had no
# extension and the problem read as unsat. And a pin after a model was made
# against the model's singletons, so any other value was refused. Found by the
# wide campaign's builder[again] check: `(y & x) == 31` on 16 bits.

def _and31():
    from tests.formal.soundness.doors import _lower
    w = 16
    b = SolveProblemBuilder()
    for i in range(2):
        b.add_var(i, width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
    b.add_constraint(_lower(b, ("=", "bool", ("bvand", w, ("var", "y", w), ("var", "x", w)),
                                ("const", 31, w)), {"x": 0, "y": 1}))
    return SolveCtx(b.finalize()[0])


def _ok(ctx):
    return ctx.get_value(0) & ctx.get_value(1) & 0xFFFF == 31


# The wall-clock deadline is the budget that stops mid-search; a restart
# limit stops at the root. With clause learning this problem runs to the
# deadline (B110), without it it is solved at once.
@pytest.mark.parametrize("budget", [dict(use_lcg=True, time_limit_ms=30),
                                    dict(max_conflicts=1, max_restarts=1)])
def test_solve_after_a_timeout(budget):
    from dv_solve.ctx import SOLVE_TIMEOUT
    with _and31() as ctx:
        gave_up = 0
        for s in range(1, 8):
            ctx.reset()
            if ctx.solve(seed=s, **budget) == SOLVE_TIMEOUT:
                gave_up += 1
                for t in range(3):
                    rc = ctx.solve(seed=100 + t)
                    assert rc == SOLVE_OK and _ok(ctx), (s, t, rc)
        if not gave_up:
            pytest.skip("no solve gave up under this budget")


def test_pin_after_a_model():
    with _and31() as ctx:
        assert ctx.solve(seed=1) == SOLVE_OK and _ok(ctx)
        x = ctx.get_value(0)
        other = (x ^ 0x100) | 31                  # also has the low five bits
        assert ctx.pin(0, other)                  # against the caller's state
        assert ctx.solve(seed=2) == SOLVE_OK and _ok(ctx) and ctx.get_value(0) == other


def test_model_stays_until_the_next_call():
    with _and31() as ctx:
        assert ctx.solve(seed=1) == SOLVE_OK
        m = (ctx.get_value(0), ctx.get_value(1))
        assert (ctx.get_value(0), ctx.get_value(1)) == m and _ok(ctx)
        cp = ctx.checkpoint()                     # discards the search
        assert ctx.solve(seed=3) == SOLVE_OK and _ok(ctx)
        ctx.restore(cp)
