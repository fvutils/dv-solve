"""Typed operators (DVS_BIN_ULT .. DVS_BIN_LSHR, docs/signed_unsigned_ops_design.md).

The operator, not the operand types, says how the operands are read: each is
extended to the wider operand's width by the operator's signedness, and
division by zero is SMT-LIB's. Each case pins the operands with equalities,
solves through the SV builder, checks the result against a Python reference,
and has the validator re-check the model. Elaboration lowers a typed operator
to SV operators on operands cast to its signedness (zero divisors guarded), so
each case runs on bit-blast and, where it compiles the problem whole, CDCL;
the validator evaluates the typed operator itself.
"""
from __future__ import annotations

import random

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import CompileIncompleteError, SolveCtx, SOLVE_OK, SOLVE_UNSAT
from dv_solve.problem import (BIN_EQ, BIN_NEQ, BIN_ULT, BIN_ULE, BIN_UGT, BIN_UGE, BIN_SLT,
                              BIN_SLE, BIN_SGT, BIN_SGE, BIN_UDIV, BIN_UREM,
                              BIN_SDIV, BIN_SREM, BIN_SMOD, BIN_LSHR)


def _u(x, w):
    return x & ((1 << w) - 1)


def _s(x, w):
    x = _u(x, w)
    return x - (1 << w) if x >> (w - 1) else x


def _tdiv(a, b):
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def _ref(op, a, b, w, wb):
    """SMT-LIB semantics of `op` on w-bit patterns a, b (b is wb bits for LSHR)."""
    ua, ub, sa, sb = _u(a, w), _u(b, w), _s(a, w), _s(b, w)
    if op == BIN_ULT: return ua < ub
    if op == BIN_ULE: return ua <= ub
    if op == BIN_UGT: return ua > ub
    if op == BIN_UGE: return ua >= ub
    if op == BIN_SLT: return sa < sb
    if op == BIN_SLE: return sa <= sb
    if op == BIN_SGT: return sa > sb
    if op == BIN_SGE: return sa >= sb
    if op == BIN_UDIV: return _u(ua // ub, w) if ub else _u(-1, w)
    if op == BIN_UREM: return ua % ub if ub else ua
    if op == BIN_SDIV:
        if sb == 0: return _u(1 if sa < 0 else -1, w)
        return _u(_tdiv(sa, sb), w)
    if op == BIN_SREM:
        if sb == 0: return ua
        return _u(sa - _tdiv(sa, sb) * sb, w)
    if op == BIN_SMOD:
        if sb == 0: return ua
        r = sa - _tdiv(sa, sb) * sb
        if r != 0 and (r < 0) != (sb < 0):
            r += sb
        return _u(r, w)
    if op == BIN_LSHR:
        n = _u(b, wb)
        return 0 if n >= w else ua >> n
    raise AssertionError(op)


CMPS = [BIN_ULT, BIN_ULE, BIN_UGT, BIN_UGE, BIN_SLT, BIN_SLE, BIN_SGT, BIN_SGE]
ARITH = [BIN_UDIV, BIN_UREM, BIN_SDIV, BIN_SREM, BIN_SMOD, BIN_LSHR]


def _k(b, v, w):
    """A w-bit unsigned constant of bit pattern v."""
    v = _u(v, w)
    if w == 64:
        return b.expr_const_wide(v, 64)
    return b.expr_const(v, False, w)


ENGINES = ("bitblast", "cdcl", "auto")
_declined = []      # (op, w): CDCL compiled it incompletely


def _solve(op, a, b, w, wb, var_signed=False, engine="bitblast"):
    """Vars x (w bits), y (wb bits) pinned to a, b; r (w bits) == x op y, or
    the constraint `x op y` for a comparison. Returns r, or the truth."""
    hi = lambda n, s: (1 << (n - 1)) - 1 if s else (-1 if n == 64 else (1 << n) - 1)  # noqa: E731
    lo = lambda n, s: -(1 << (n - 1)) if s else 0                                      # noqa: E731
    bld = SolveProblemBuilder()
    try:
        bld.add_var(0, w, var_signed, lo(w, var_signed), hi(w, var_signed))
        bld.add_var(1, wb, var_signed, lo(wb, var_signed), hi(wb, var_signed))
        bld.add_var(2, w, False, 0, hi(w, False))
        x, y = bld.expr_var(0), bld.expr_var(1)
        # pin through unsigned bit patterns: x[w-1:0] == a
        bld.add_constraint(bld.expr_binary(BIN_EQ, x, _k(bld, a, w)))
        bld.add_constraint(bld.expr_binary(BIN_EQ, y, _k(bld, b, wb)))
        node = bld.expr_binary(op, x, y)
        if op in CMPS:
            bld.add_constraint(node)
        else:
            bld.add_constraint(bld.expr_binary(BIN_EQ, bld.expr_var(2), node))
        buf, _sz = bld.finalize()
    finally:
        bld.destroy()
    try:
        ctx = SolveCtx(buf, engine=engine)
    except CompileIncompleteError:
        assert engine == "cdcl"
        _declined.append((op, w))
        return None
    try:
        rc = ctx.solve(seed=1)
        if op in CMPS:
            assert rc in (SOLVE_OK, SOLVE_UNSAT)
            if rc == SOLVE_OK:
                assert ctx.validate_model() == 0
            return rc == SOLVE_OK
        assert rc == SOLVE_OK
        assert ctx.validate_model() == 0
        return _u(ctx.get_value(2), w)
    finally:
        ctx.destroy()


def _edges(w):
    return sorted({0, 1, 2, 3, _u(-1, w), _u(-2, w), (1 << (w - 1)) if w > 1 else 1,
                   _u((1 << (w - 1)) - 1, w), _u((1 << (w - 1)) + 1, w)})


def _cases(w, n=6, seed=0):
    r = random.Random(seed * 1000 + w)
    e = _edges(w)
    out = [(r.choice(e), r.choice(e)) for _ in range(n)]
    out += [(r.getrandbits(w), r.getrandbits(w)) for _ in range(n)]
    out += [(r.getrandbits(w), 0), (_u(1 << (w - 1), w), _u(-1, w))]    # x/0, MIN/-1
    return out


@pytest.mark.parametrize("engine", ENGINES)
@pytest.mark.parametrize("w", [1, 8, 63, 64])
@pytest.mark.parametrize("op", CMPS + ARITH)
def test_typed_op_matches_smtlib(op, w, engine):
    for a, b in _cases(w, seed=op):
        got = _solve(op, a, b, w, w, engine=engine)
        if got is None:
            continue                    # CDCL declined it (see test_cdcl_coverage)
        want = _ref(op, a, b, w, w)
        assert got == want, (op, w, engine, hex(a), hex(b), got, want)


def test_cdcl_coverage():
    """CDCL compiles every typed operator at 8 and 32 bits. At 63-64 it
    declines what its int64 domains can't hold safely (design step 4 lifts
    this): there the signed division family was wrongly UNSAT before the
    decline."""
    _declined.clear()
    for op in CMPS + ARITH:
        for w in (8, 32):
            for a, b in _cases(w, n=2, seed=op):
                _solve(op, a, b, w, w, engine="cdcl")
    assert not _declined, sorted(set(_declined))


@pytest.mark.parametrize("engine", ENGINES)
@pytest.mark.parametrize("op", [BIN_ULT, BIN_SLT, BIN_UDIV, BIN_SDIV, BIN_SMOD])
def test_narrow_operand_is_extended_by_the_operator(op, engine):
    """An 8-bit operand against a 16-bit one: zero-extended under U*, sign-
    extended under S*, whatever the variable's own signedness."""
    for var_signed in (False, True):
        for a, b in [(0x00F0, 0x80), (0x7FFF, 0xFF), (0x8000, 0x7F), (5, 0xFE)]:
            # x is 16 bits, y 8 bits: reference extends y by the operator
            yb = _u(_s(b, 8), 16) if op in (BIN_SLT, BIN_SDIV, BIN_SMOD) else b
            got = _solve(op, a, b, 16, 8, var_signed, engine)
            if got is None:
                continue
            want = _ref(op, a, yb, 16, 16)
            assert got == want, (op, var_signed, hex(a), hex(b), got, want)


@pytest.mark.parametrize("engine", ENGINES)
def test_lshr_amount_is_its_own_width(engine):
    for a, n in [(0xF0, 4), (0xF0, 8), (0xF0, 200), (0x80, 7)]:
        got = _solve(BIN_LSHR, a, n, 8, 9, engine=engine)
        assert got in (None, _ref(BIN_LSHR, a, n, 8, 9)), (a, n, engine, got)


def _problem(op, a, b, w, r_rel, rel=BIN_EQ):
    """x, y pinned to a, b and r pinned to `r_rel`; plus `r rel (x op y)`."""
    bld = SolveProblemBuilder()
    try:
        for i in range(3):
            bld.add_var(i, w, False, 0, (1 << w) - 1)
        x, y, r = bld.expr_var(0), bld.expr_var(1), bld.expr_var(2)
        bld.add_constraint(bld.expr_binary(BIN_EQ, x, _k(bld, a, w)))
        bld.add_constraint(bld.expr_binary(BIN_EQ, y, _k(bld, b, w)))
        bld.add_constraint(bld.expr_binary(BIN_EQ, r, _k(bld, r_rel, w)))
        bld.add_constraint(bld.expr_binary(rel, r, bld.expr_binary(op, x, y)))
        buf, _sz = bld.finalize()
    finally:
        bld.destroy()
    return buf


@pytest.mark.parametrize("op", ARITH)
def test_validator_evaluates_typed_ops(op):
    """The validator evaluates a typed operator rather than skipping it: the
    model, checked against `r != x op y`, reports that one violation (a
    skipped constraint would count none)."""
    import ctypes
    a, b, w = 0xF9, 0x03, 8
    want = _ref(op, a, b, w, w)
    good = _problem(op, a, b, w, want)
    ctx = SolveCtx(good)
    try:
        assert ctx.solve(seed=1) == SOLVE_OK and ctx.validate_model() == 0
        bad = _problem(op, a, b, w, want, rel=BIN_NEQ)
        n = ctx._lib.dvs_solver_validate_model(
            ctx._ctx, ctypes.cast(bad, ctypes.c_void_p).value, None)
        assert n == 1, n
    finally:
        ctx.destroy()


@pytest.mark.parametrize("op", CMPS + ARITH)
def test_oracle_confirms_typed_ops(op, tmp_path):
    """z3, through the SMT-LIB emitter (DV oracle), agrees with every
    answer, at the edges and with a narrow signed operand."""
    import shutil
    if not shutil.which("z3"):
        pytest.skip("z3 not on PATH")
    from dv_solve.oracle import Oracle
    with Oracle(out_dir=str(tmp_path / "orc"), raise_on_fail=True) as orc:
        for (a, b, w, wb, sg) in [(0xF9, 0x03, 8, 8, False), (0x80, 0xFF, 8, 8, True),
                                  (0x8000, 0x7F, 16, 8, True), (0xF0, 0, 8, 8, False),
                                  ((1 << 63), (1 << 64) - 1, 64, 64, False)]:
            bld = SolveProblemBuilder()
            try:
                bld.add_var(0, w, sg, -(1 << (w - 1)) if sg else 0,
                            (1 << (w - 1)) - 1 if sg else (-1 if w == 64 else (1 << w) - 1))
                bld.add_var(1, wb, sg, -(1 << (wb - 1)) if sg else 0,
                            (1 << (wb - 1)) - 1 if sg else (-1 if wb == 64 else (1 << wb) - 1))
                bld.add_var(2, w, False, 0, -1 if w == 64 else (1 << w) - 1)
                x, y = bld.expr_var(0), bld.expr_var(1)
                bld.add_constraint(bld.expr_binary(BIN_EQ, x, _k(bld, a, w)))
                bld.add_constraint(bld.expr_binary(BIN_EQ, y, _k(bld, b, wb)))
                node = bld.expr_binary(op, x, y)
                bld.add_constraint(node if op in CMPS
                                   else bld.expr_binary(BIN_EQ, bld.expr_var(2), node))
                buf, _sz = bld.finalize()
            finally:
                bld.destroy()
            ctx = SolveCtx(buf, oracle=orc)
            try:
                ctx.solve(seed=1)
            finally:
                ctx.destroy()
        st = orc.stats()
    assert st["bad_model"] == 0 and st["bad_unsat"] == 0 and st["error"] == 0, st
    assert st["ok"] == 5, st
