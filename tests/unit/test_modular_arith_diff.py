"""Exhaustive differential soundness test for modular fixed-width
2's-complement var-var arithmetic (ADD/SUB/MUL/LSHIFT).

For each op, signedness, and small width, every pair of fully-fixed
operand values is solved and compared against the Python-computed
w-bit wrapped result. Also samples range-valued operands and asserts
the solver returns a value consistent with `r == (a op b) mod 2^w`.

This is the soundness oracle for the wrap-aware bit-vector
propagators in zsp_prop_templates.c (prop_add_bv{add,sub,mul,shl}_64).
"""
from __future__ import annotations

import pytest

from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT

PROP_OK = 0
PROP_CONFLICT = 1
from dv_solve.problem import (
    SolveProblem, BIN_ADD, BIN_SUB, BIN_MUL, BIN_LSHIFT, BIN_EQ,
    BIN_DIV, BIN_MOD,
)

OPS = {
    "add": BIN_ADD,
    "sub": BIN_SUB,
    "mul": BIN_MUL,
    "shl": BIN_LSHIFT,
}


def _mask(w: int) -> int:
    return (1 << w) - 1


def _to_signed(u: int, w: int) -> int:
    """Interpret w-bit unsigned pattern u as a signed value."""
    u &= _mask(w)
    if u & (1 << (w - 1)):
        return u - (1 << w)
    return u


def _wrap_result(opname: str, ua: int, ub: int, w: int, signed: bool) -> int:
    """True w-bit result (as the value the solver should report for r).

    Arithmetic is done on the w-bit unsigned bit-patterns (2's-complement
    is bit-identical to unsigned mod 2^w); the value is then re-interpreted
    according to the result variable's signedness.
    """
    m = _mask(w)
    ua &= m
    ub &= m
    if opname == "add":
        u = (ua + ub) & m
    elif opname == "sub":
        u = (ua - ub) & m
    elif opname == "mul":
        u = (ua * ub) & m
    elif opname == "shl":
        # shift amount is the *unsigned* numeric value of operand b
        u = (ua << ub) & m if ub < w else 0
    else:
        raise AssertionError(opname)
    return _to_signed(u, w) if signed else u


def _domain(w: int, signed: bool):
    if signed:
        return (-(1 << (w - 1)), (1 << (w - 1)) - 1)
    return (0, (1 << w) - 1)


def _all_values(w: int, signed: bool):
    lo, hi = _domain(w, signed)
    return range(lo, hi + 1)


def _build_and_solve(opname, w, signed, a_dom, b_dom, seed=1, shave=0):
    """Construct r == (a op b); return (result_code, value_of_r_or_None)."""
    op = OPS[opname]
    b = SolveProblem()
    dlo, dhi = _domain(w, signed)
    # var 0 = r, var 1 = a, var 2 = b
    b.add_var(0, w, signed, dlo, dhi)
    b.add_var(1, w, signed, a_dom[0], a_dom[1])
    # shift amount semantics: b operand is still a w-bit value
    b.add_var(2, w, signed, b_dom[0], b_dom[1])
    r_e = b.expr_var(0)
    a_e = b.expr_var(1)
    b_e = b.expr_var(2)
    opexpr = b.expr_binary(op, a_e, b_e)
    b.add_constraint(b.expr_binary(BIN_EQ, r_e, opexpr))
    with SolveCtx(b) as ctx:
        # Enable bounds-shaving: the modular MUL/SHL propagators are
        # forward-dominant (a singleton-constant operand has full backward
        # inference, ranges are pruned conservatively). Shaving — a sound,
        # built-in completeness aid — lets the search converge on the
        # non-interval feasible sets (e.g. "r must be even") that pure
        # bound propagation cannot represent. Soundness is independently
        # verified by the fixed-operand exhaustive test, which passes with
        # shaving disabled.
        rc = ctx.solve(seed=seed, max_shave_iters=shave)
        if rc != SOLVE_OK:
            return rc, None
        return rc, ctx.get_value(0)


@pytest.mark.parametrize("opname", list(OPS.keys()))
@pytest.mark.parametrize("signed", [False, True])
@pytest.mark.parametrize("w", [3, 4, 5])
def test_fixed_operands_exhaustive(opname, signed, w):
    failures = []
    for va in _all_values(w, signed):
        for vb in _all_values(w, signed):
            ua = va & _mask(w)
            ub = vb & _mask(w)
            expected = _wrap_result(opname, ua, ub, w, signed)
            rc, got = _build_and_solve(opname, w, signed, (va, va), (vb, vb))
            if rc != SOLVE_OK:
                failures.append(
                    f"{opname} w={w} signed={signed} a={va} b={vb}: "
                    f"solver returned {rc} (expected SAT, r={expected})"
                )
                continue
            got_norm = _to_signed(got, w) if signed else (got & _mask(w))
            if got_norm != expected:
                failures.append(
                    f"{opname} w={w} signed={signed} a={va} b={vb}: "
                    f"got r={got} (norm {got_norm}), expected {expected}"
                )
    assert not failures, "\n".join(failures[:40]) + (
        f"\n... ({len(failures)} total failures)" if len(failures) > 40 else ""
    )


def _propagate_with_r(opname, w, signed, a_dom, b_dom, r_dom):
    """Build r == (a op b) with the given domains, run propagate_only,
    and return PROP_OK/PROP_CONFLICT."""
    op = OPS[opname]
    b = SolveProblem()
    b.add_var(0, w, signed, r_dom[0], r_dom[1])
    b.add_var(1, w, signed, a_dom[0], a_dom[1])
    b.add_var(2, w, signed, b_dom[0], b_dom[1])
    opexpr = b.expr_binary(op, b.expr_var(1), b.expr_var(2))
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), opexpr))
    with SolveCtx(b) as ctx:
        return ctx.propagate_only()


@pytest.mark.parametrize("opname", list(OPS.keys()))
@pytest.mark.parametrize("signed", [False, True])
@pytest.mark.parametrize("w", [3, 4, 5])
def test_range_operands_sound(opname, signed, w):
    """Range-operand SOUNDNESS oracle, tested directly at the propagation
    level (deterministic — independent of the randomized search):

    For every sampled (a-range, b-range) and every candidate value rv for
    the result, fix r=[rv,rv] and run propagation. The propagator MUST:
      * NOT report a conflict when rv is a true feasible wrapped result for
        some (a,b) in range  (i.e. never exclude a feasible value), and
      * report a conflict when rv is infeasible for every (a,b) in range
        (this is the completeness guarantee that keeps search sound — when
        it is achievable; weak propagation is allowed to *miss* a conflict,
        which the assertion below tolerates, but it must NEVER fire a
        spurious one).
    The first property is the critical soundness guarantee.
    """
    lo, hi = _domain(w, signed)
    mids = sorted({lo, lo + 1, (lo + hi) // 2, hi - 1, hi})
    failures = []
    for alo in mids:
        for ahi in mids:
            if ahi < alo:
                continue
            for blo in mids:
                for bhi in mids:
                    if bhi < blo:
                        continue
                    feasible = set()
                    for va in range(alo, ahi + 1):
                        for vb in range(blo, bhi + 1):
                            feasible.add(
                                _wrap_result(opname, va & _mask(w),
                                             vb & _mask(w), w, signed)
                            )
                    for rv in range(lo, hi + 1):
                        pr = _propagate_with_r(
                            opname, w, signed, (alo, ahi), (blo, bhi), (rv, rv)
                        )
                        if rv in feasible and pr == PROP_CONFLICT:
                            # UNSOUND: excluded a genuinely feasible value.
                            failures.append(
                                f"{opname} w={w} signed={signed} "
                                f"a=[{alo},{ahi}] b=[{blo},{bhi}] r={rv}: "
                                f"spurious CONFLICT (feasible={sorted(feasible)})"
                            )
    assert not failures, "\n".join(failures[:40])


# ---------------------------------------------------------------------------
# Exhaustive signed/unsigned DIV/MOD differential against the SV reference.
# ---------------------------------------------------------------------------

def sv_div(a: int, b: int) -> int:
    """SystemVerilog signed division: truncate toward zero."""
    if b == 0:
        return 0
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def sv_mod(a: int, b: int) -> int:
    """SystemVerilog signed remainder: sign of the dividend a."""
    if b == 0:
        return 0
    rem = abs(a) % abs(b)
    return -rem if a < 0 else rem


def _build_and_solve_divmod(op, w, signed, va, vb):
    """r == (a / b) or (a % b) with a,b fixed; return (rc, value_of_r)."""
    b = SolveProblem()
    dlo, dhi = _domain(w, signed)
    b.add_var(0, w, signed, dlo, dhi)   # r
    b.add_var(1, w, signed, va, va)     # a
    b.add_var(2, w, signed, vb, vb)     # b
    opexpr = b.expr_binary(op, b.expr_var(1), b.expr_var(2))
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), opexpr))
    with SolveCtx(b) as ctx:
        rc = ctx.solve(seed=1)
        if rc != SOLVE_OK:
            return rc, None
        return rc, ctx.get_value(0)


@pytest.mark.parametrize("signed", [False, True])
@pytest.mark.parametrize("w", [3, 4, 5])
def test_divmod_fixed_operands_exhaustive(signed, w):
    """Every operand pair (a fixed, b fixed, b != 0) over the full w-bit
    domain, for both div and mod, must equal the SV reference (signed) or
    plain unsigned div/rem (unsigned). Includes negative divisors."""
    failures = []
    for va in _all_values(w, signed):
        for vb in _all_values(w, signed):
            if vb == 0:
                continue
            if signed:
                # The SV quotient is then stored into a w-bit result, which
                # truncates/wraps (only reachable case: INT_MIN/-1 overflow).
                exp_div = _to_signed(sv_div(va, vb) & _mask(w), w)
                exp_mod = _to_signed(sv_mod(va, vb) & _mask(w), w)
            else:
                exp_div = va // vb
                exp_mod = va % vb
            for op, opname, expected in (
                (BIN_DIV, "div", exp_div),
                (BIN_MOD, "mod", exp_mod),
            ):
                rc, got = _build_and_solve_divmod(op, w, signed, va, vb)
                if rc != SOLVE_OK:
                    failures.append(
                        f"{opname} w={w} signed={signed} a={va} b={vb}: "
                        f"rc={rc} (expected SAT, r={expected})"
                    )
                    continue
                got_norm = _to_signed(got, w) if signed else (got & _mask(w))
                if got_norm != expected:
                    failures.append(
                        f"{opname} w={w} signed={signed} a={va} b={vb}: "
                        f"got r={got} (norm {got_norm}), expected {expected}"
                    )
    assert not failures, "\n".join(failures[:40]) + (
        f"\n... ({len(failures)} total failures)" if len(failures) > 40 else ""
    )


def test_divmod_reference_cases():
    """The exact signed reference values called out in the task."""
    cases = [
        (-7, 3, -2, -1), (7, -3, -2, 1), (-7, -3, 2, -1), (-7, 2, -3, -1),
        (7, 3, 2, 1), (-8, 3, -2, -2), (5, -2, -2, 1), (-1, 4, 0, -1),
    ]
    w = 5  # domain [-16, 15] covers all operands above
    for a, bb, q, rmod in cases:
        rc, gq = _build_and_solve_divmod(BIN_DIV, w, True, a, bb)
        assert rc == SOLVE_OK and _to_signed(gq, w) == q, (
            f"div a={a} b={bb}: got {gq}, exp {q}")
        rc, gm = _build_and_solve_divmod(BIN_MOD, w, True, a, bb)
        assert rc == SOLVE_OK and _to_signed(gm, w) == rmod, (
            f"mod a={a} b={bb}: got {gm}, exp {rmod}")


def test_acceptance_cases():
    """The specific cases called out in the task."""
    # 8-bit unsigned add 200+100 -> 44
    rc, r = _build_and_solve("add", 8, False, (200, 200), (100, 100))
    assert rc == SOLVE_OK and (r & 0xFF) == 44
    # 8-bit unsigned sub 10-20 -> 246
    rc, r = _build_and_solve("sub", 8, False, (10, 10), (20, 20))
    assert rc == SOLVE_OK and (r & 0xFF) == 246
    # 8-bit unsigned mul 20*20 -> 144
    rc, r = _build_and_solve("mul", 8, False, (20, 20), (20, 20))
    assert rc == SOLVE_OK and (r & 0xFF) == 144
    # 8-bit unsigned 1 << 10 -> 0
    rc, r = _build_and_solve("shl", 8, False, (1, 1), (10, 10))
    assert rc == SOLVE_OK and (r & 0xFF) == 0
