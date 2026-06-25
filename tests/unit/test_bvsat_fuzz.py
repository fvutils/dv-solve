"""Differential / oracle fuzzer for the BV-SAT engine (Phase A, T-F).

Generates random *small* scalar constraint systems, computes ground truth by
brute force (enumerating the whole domain — feasible because widths/var-counts
are kept tiny), and asserts:

  * BV-SAT reports SAT iff the system is actually satisfiable, and every SAT
    model it returns satisfies all constraints;
  * the primary bounds-propagation engine never *contradicts* ground truth
    (it may return non-OK = incomplete, but if it returns a model that model
    must be valid, and it must not claim SAT on an unsatisfiable system).

Brute force is the oracle, so no external solver (Boolector) is needed. All
randomness is seeded → reproducible.
"""
from __future__ import annotations

import random

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import (
    BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE, BIN_GT, BIN_GTE,
    BIN_ADD, BIN_SUB, BIN_BAND, BIN_BOR, BIN_BXOR,
)
from dv_solve.bvsat import BVSatCtx, BVSAT_SAT, BVSAT_UNSAT
from dv_solve.ctx import SolveCtx, SOLVE_OK, CompileUnsatError, CompileIncompleteError


_CMP_OPS = [BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE, BIN_GT, BIN_GTE]
_ARITH_OPS = [BIN_ADD, BIN_SUB, BIN_BAND, BIN_BOR, BIN_BXOR]


def _eval_arith(op, a, b, mask):
    if op == BIN_ADD:  return (a + b) & mask
    if op == BIN_SUB:  return (a - b) & mask
    if op == BIN_BAND: return a & b
    if op == BIN_BOR:  return a | b
    if op == BIN_BXOR: return a ^ b
    raise AssertionError("bad arith op")


def _eval_cmp(op, a, b):
    if op == BIN_EQ:  return a == b
    if op == BIN_NEQ: return a != b
    if op == BIN_LT:  return a < b
    if op == BIN_LTE: return a <= b
    if op == BIN_GT:  return a > b
    if op == BIN_GTE: return a >= b
    raise AssertionError("bad cmp op")


class _Operand:
    """A width-w unsigned operand: a bare variable or a constant.

    Deliberately *not* nested arithmetic: an expression like ``v + c`` is
    widened by the bit-blaster to avoid overflow (so ``15 + 1`` compares as 16,
    not 0), and replicating that width-context faithfully in a Python oracle is
    error-prone. Bare operands compare two width-w values with unambiguous
    semantics, giving a bulletproof brute-force oracle. Arithmetic op
    correctness is covered by the value-checked cases in test_bvsat.py."""
    def __init__(self, rng, nvars, mask):
        self.mask = mask
        if rng.randint(0, 1) == 0:
            self.var = rng.randrange(nvars); self.const = None
        else:
            self.var = None; self.const = rng.randint(0, mask)

    def eval(self, vals):
        return (self.const if self.var is None else vals[self.var]) & self.mask

    def build(self, b):
        if self.var is None:
            return b.expr_const(self.const)
        return b.expr_var(self.var)


class _Constraint:
    """A top-level predicate: operand CMP operand."""
    def __init__(self, rng, nvars, mask):
        self.lhs = _Operand(rng, nvars, mask)
        self.rhs = _Operand(rng, nvars, mask)
        self.cmp = rng.choice(_CMP_OPS)

    def eval(self, vals):
        return _eval_cmp(self.cmp, self.lhs.eval(vals), self.rhs.eval(vals))

    def build(self, b):
        return b.expr_binary(self.cmp, self.lhs.build(b), self.rhs.build(b))


def _brute_force_sat(nvars, w, constraints):
    """Return (any_sat, set_of_satisfying_tuples)."""
    domain = 1 << w
    sols = []
    # Enumerate the full cross product (kept small by construction).
    def rec(prefix):
        if len(prefix) == nvars:
            if all(c.eval(prefix) for c in constraints):
                sols.append(tuple(prefix))
            return
        for v in range(domain):
            prefix.append(v)
            rec(prefix)
            prefix.pop()
    rec([])
    return (len(sols) > 0), set(sols)


def _build_problem(nvars, w, constraints):
    b = SolveProblemBuilder()
    mask = (1 << w) - 1
    for i in range(nvars):
        b.add_var(i, w, False, 0, mask)
    for c in constraints:
        b.add_constraint(c.build(b))
    buf, _sz = b.finalize()
    return b, buf


def test_bvsat_fuzz_vs_bruteforce():
    rng = random.Random(0xC0FFEE)
    N = 250
    for it in range(N):
        nvars = rng.randint(2, 3)
        w = rng.randint(3, 5)               # domain <= 32^3 ~= 32k
        if nvars == 3 and w > 4:            # cap total enumeration
            w = 4
        nc = rng.randint(2, 5)
        cons = [_Constraint(rng, nvars, (1 << w) - 1) for _ in range(nc)]

        truth_sat, sols = _brute_force_sat(nvars, w, cons)

        b, buf = _build_problem(nvars, w, cons)
        try:
            # --- BV-SAT must match ground truth exactly. ---
            bb = BVSatCtx(buf)
            try:
                rc = bb.check(seed=(it * 2654435761) & 0x7FFFFFFF)
                if truth_sat:
                    assert rc == BVSAT_SAT, \
                        "iter %d: satisfiable but BV-SAT said rc=%d" % (it, rc)
                    model = tuple(bb.value(i) for i in range(nvars))
                    assert model in sols, \
                        "iter %d: BV-SAT model %s not a real solution" % (it, model)
                else:
                    assert rc == BVSAT_UNSAT, \
                        "iter %d: UNSAT but BV-SAT said rc=%d" % (it, rc)
            finally:
                bb.destroy()

            # --- Primary engine must never *contradict* ground truth. ---
            # It is allowed to be incomplete (return non-OK on a SAT system,
            # or raise CompileIncompleteError), but it must not (a) claim SAT
            # on an UNSAT system, nor (b) return an invalid model. This is the
            # cross-check Phase A omitted; the soundness fix re-enables it.
            try:
                ctx = SolveCtx(buf)
            except CompileUnsatError:
                assert not truth_sat, \
                    "iter %d: primary said UNSAT (compile) but system is SAT" % it
                ctx = None
            except CompileIncompleteError:
                ctx = None  # incomplete: no claim either way — allowed
            if ctx is not None:
                try:
                    prc = ctx.solve(seed=(it * 40503 + 1) & 0x7FFFFFFF,
                                    fair_pick=True)
                    if prc == SOLVE_OK:
                        pmodel = tuple(ctx.get_value(i) for i in range(nvars))
                        assert pmodel in sols, \
                            "iter %d: primary model %s not a real solution " \
                            "(truth_sat=%s)" % (it, pmodel, truth_sat)
                finally:
                    ctx.destroy()
        finally:
            b.destroy()


_INT64_MAX = (1 << 63) - 1


def _repr_range(w, signed):
    """Representable [min, max] for a width-`w` (un)signed variable.

    The C add-var / const API carries bounds as int64_t, so an unsigned
    2^w-1 that exceeds int64 (w >= 64) is clamped to INT64_MAX — which is
    exactly what `var_repr_max` reports for such vars, keeping the declared
    domain and the guard's notion of the edge consistent."""
    if signed:
        return -(1 << (w - 1)), (1 << (w - 1)) - 1
    return 0, min((1 << w) - 1, _INT64_MAX)


def _primary_is_unsat(nvars_specs, constraints):
    """Build a problem (specs: list of (vid,w,signed,lo,hi); constraints:
    list of (op, lhs_expr_fn, rhs_expr_fn)) and run the *primary* engine.
    Returns True iff the primary proves UNSAT (compile-time or search)."""
    b = SolveProblemBuilder()
    for (vid, w, signed, lo, hi) in nvars_specs:
        b.add_var(vid, w, signed, lo, hi)
    for (op, lhs, rhs) in constraints:
        b.add_constraint(b.expr_binary(op, lhs(b), rhs(b)))
    buf, _sz = b.finalize()
    try:
        try:
            ctx = SolveCtx(buf)
        except CompileUnsatError:
            return True  # proven UNSAT at compile time
        try:
            return ctx.solve(seed=1, fair_pick=True) != SOLVE_OK
        finally:
            ctx.destroy()
    finally:
        b.destroy()


_SOUNDNESS_WIDTHS = [1, 4, 8, 16, 32, 33, 64]


@pytest.mark.parametrize("w", _SOUNDNESS_WIDTHS)
@pytest.mark.parametrize("signed", [False, True])
def test_primary_always_false_strict_compare_is_unsat(w, signed):
    """An always-false *strict* comparison that pushes a bound past the
    variable's representable edge must be UNSAT, not SOLVE_OK with garbage.

    This was the T-F fuzzer's soundness finding: `0 > v` / `v < 0` (unsigned)
    tightened the upper bound to -1, which was reinterpreted as a huge unsigned
    bound, silently dropping the constraint. Now guarded in the 64-bit tighten
    functions (`_var_repr_min` / `_var_repr_max`)."""
    from dv_solve.problem import BIN_GT, BIN_LT
    vmin, vmax = _repr_range(w, signed)

    # `v < vmin`  (impossible) — both operand orders.
    assert _primary_is_unsat(
        [(0, w, signed, vmin, vmax)],
        [(BIN_LT, lambda b: b.expr_var(0), lambda b: b.expr_const(vmin))],
    ), "v < min should be UNSAT (w=%d signed=%s)" % (w, signed)

    # `vmin > v`  (same predicate, const on the left → exercises the swap).
    assert _primary_is_unsat(
        [(0, w, signed, vmin, vmax)],
        [(BIN_GT, lambda b: b.expr_const(vmin), lambda b: b.expr_var(0))],
    ), "min > v should be UNSAT (w=%d signed=%s)" % (w, signed)

    # `v > vmax`  (impossible) — the symmetric lower-bound edge.
    assert _primary_is_unsat(
        [(0, w, signed, vmin, vmax)],
        [(BIN_GT, lambda b: b.expr_var(0), lambda b: b.expr_const(vmax))],
    ), "v > max should be UNSAT (w=%d signed=%s)" % (w, signed)


@pytest.mark.parametrize("w", _SOUNDNESS_WIDTHS)
@pytest.mark.parametrize("signed", [False, True])
def test_primary_tight_edge_compare_is_sat(w, signed):
    """The guard must not over-trigger: a *satisfiable* compare that lands
    exactly on the representable edge (`v < min+1` → v==min, `v > max-1` →
    v==max) must still be SAT. Single-element domains, not empty ones."""
    from dv_solve.problem import BIN_GT, BIN_LT
    vmin, vmax = _repr_range(w, signed)
    if vmin == vmax:
        pytest.skip("degenerate single-value domain (w=1 has no interior)")

    # `v < vmin+1` → v == vmin (SAT).
    assert not _primary_is_unsat(
        [(0, w, signed, vmin, vmax)],
        [(BIN_LT, lambda b: b.expr_var(0), lambda b: b.expr_const(vmin + 1))],
    ), "v < min+1 should be SAT (w=%d signed=%s)" % (w, signed)

    # `v > vmax-1` → v == vmax (SAT).
    assert not _primary_is_unsat(
        [(0, w, signed, vmin, vmax)],
        [(BIN_GT, lambda b: b.expr_var(0), lambda b: b.expr_const(vmax - 1))],
    ), "v > max-1 should be SAT (w=%d signed=%s)" % (w, signed)
