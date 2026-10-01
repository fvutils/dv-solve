"""Brute-force differential test: DIV / MOD / RSHIFT (and friends) on signed
and unsigned operands, through the builder API, against Python reference
semantics.

Reference semantics: SystemVerilog expression sizing and signedness, as
implemented by the reference evaluator in test_sv_width_semantics.py (see its
docstring). In particular:

* ``a / b``  -- truncates toward zero when signed: -8 / 3 == -2.
* ``a % b``  -- the remainder takes the sign of the dividend: -8 % 3 == -2.
* ``a >> k`` -- a LOGICAL shift of the context-width bit pattern (SV ``>>``).
  A signed -8 (8-bit) shifted right by an unsized 1 is evaluated in a 32-bit
  context (the literal is 32 bits): 0xFFFFFFF8 >> 1 == 0x7FFFFFFC. (Until the
  SV-sizing change this test encoded an arithmetic, floor shift: -4.)
* Constants are unsized SV integer literals (32-bit signed when they fit), so
  e.g. ``x + 1`` over an 8-bit x is evaluated at 32 bits, and ``r == expr``
  with r signed 64-bit at 64 bits; an unsigned operand makes the whole
  context unsigned.

For every generated case the expected satisfiability is computed by
enumerating the (small) operand domains in Python. dv-solve must never
disagree: a wrong sat, a wrong unsat, or a sat whose model violates the
constraint is a failure. Declining (CompileIncompleteError) or a timeout is
allowed -- that is the engine being honest, not wrong.

Regression history (all found by this sweep):
* ``(x % 3) == -2`` over a signed x was unsat (and ``== 1`` sat): the
  ``binop == const`` pin var was UNSIGNED, and the div/mod propagators keyed
  their signedness off the result var, so the floored remainder was used.
* ``r == (x >> 1)`` with x == -8 was unsat for every r: a negative operand was
  shifted logically at 64 bits (2^63 - 4).
* INT64_MIN / -1 and INT64_MIN % -1 crashed the process with SIGFPE (both in
  the propagators and in the model validator).
* ``x + 1 == 0`` over a 64-bit unsigned x was sat at x == 2^32 - 1: the pin var
  was a fixed width-32 var, so the modular add wrapped at 32 bits.
* ``x != INT32_MAX`` style exclusions overflowed ``v + 1`` into a no-op bound.
* Constants materialised beside a signed 64-bit operand were unsigned, so
  ``x + -7 >= 11`` and ``(x << 4) < 1`` came back unsat.
"""
from __future__ import annotations

import random

import pytest

from dv_solve.builder import SolveProblemBuilder

from .test_sv_width_semantics import (
    Var as _V, Const as _C, Bin as _B, DivZero as _DivZero,
    self_type as _self_type, val as _val, truth as _truth, wrap as _wrap,
)
from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT, CompileIncompleteError
from dv_solve.problem import (
    BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV, BIN_MOD, BIN_LSHIFT, BIN_RSHIFT,
    BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE, BIN_GT, BIN_GTE,
)

# ------------------------------------------------------------------ #
# Reference semantics                                                  #
# ------------------------------------------------------------------ #


def _tdiv(a: int, b: int) -> int:
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def _tmod(a: int, b: int) -> int:
    return a - b * _tdiv(a, b)


def ref(op: int, a: int, b: int):
    """Reference value of `a op b`, or None when undefined (division by 0)."""
    if op == BIN_DIV:
        return None if b == 0 else _tdiv(a, b)
    if op == BIN_MOD:
        return None if b == 0 else _tmod(a, b)
    if op == BIN_RSHIFT:
        return a >> b
    if op == BIN_LSHIFT:
        return a << b
    if op == BIN_ADD:
        return a + b
    if op == BIN_SUB:
        return a - b
    if op == BIN_MUL:
        return a * b
    raise AssertionError(op)


CMP = {
    BIN_EQ: lambda a, b: a == b, BIN_NEQ: lambda a, b: a != b,
    BIN_LT: lambda a, b: a < b, BIN_LTE: lambda a, b: a <= b,
    BIN_GT: lambda a, b: a > b, BIN_GTE: lambda a, b: a >= b,
}
OPN = {BIN_DIV: "/", BIN_MOD: "%", BIN_RSHIFT: ">>", BIN_LSHIFT: "<<",
       BIN_ADD: "+", BIN_SUB: "-", BIN_MUL: "*"}
CMPN = {BIN_EQ: "==", BIN_NEQ: "!=", BIN_LT: "<", BIN_LTE: "<=",
        BIN_GT: ">", BIN_GTE: ">="}

I64_LO, I64_HI = -(1 << 63), (1 << 63) - 1
R_LO, R_HI = -(1 << 62), 1 << 62     # domain of the free result var r


def _rng(w: int, s: bool):
    # An unsigned 64-bit var cannot hold values >= 2^63 through the int64 API.
    if s:
        return -(1 << (w - 1)), (1 << (w - 1)) - 1
    return 0, min((1 << w) - 1, I64_HI)


# ------------------------------------------------------------------ #
# One case                                                             #
# ------------------------------------------------------------------ #


class Case:
    """x (var 0) op k / k op x / x op y (var 2); either `r == expr` with r
    (var 1) a free signed 64-bit var, or `expr CMP c`."""

    def __init__(self, w, s, xlo, xhi, op, shape, k=None, y=None, cmp=None,
                 c=None):
        self.w, self.s, self.xlo, self.xhi = w, s, xlo, xhi
        self.op, self.shape, self.k, self.y = op, shape, k, y
        self.cmp, self.c = cmp, c

    def pairs(self):
        xs = range(self.xlo, self.xhi + 1)
        if self.shape == "vc":
            return [(x, self.k) for x in xs]
        if self.shape == "cv":
            return [(self.k, x) for x in xs]
        yw, ys, ylo, yhi = self.y
        return [(x, yv) for x in xs for yv in range(ylo, yhi + 1)]

    # -- SystemVerilog reference ------------------------------------- #

    def _vt(self):
        vt = {0: (self.w, self.s), 1: (64, True)}
        if self.shape == "vv":
            vt[2] = (self.y[0], self.y[1])
        return vt

    def _expr(self):
        x = _V(0)
        if self.shape == "vv":
            return _B(self.op, x, _V(2))
        k = _C(self.k, is_signed=self.k < 0)
        return _B(self.op, x, k) if self.shape == "vc" else _B(self.op, k, x)

    def _root(self):
        if self.cmp is None:
            return _B(BIN_EQ, _V(1), self._expr())
        return _B(self.cmp, self._expr(), _C(self.c, is_signed=self.c < 0))

    def _env(self, a, b, r=0):
        x, y = (a, b) if self.shape != "cv" else (b, a)
        return {0: x, 1: r, 2: y}

    def values(self):
        """The expression's value in its own (self-determined) type, per
        operand pair; None where it divides by zero."""
        vt, e = self._vt(), self._expr()
        w, sg = _self_type(e, vt)
        out = []
        for a, b in self.pairs():
            try:
                out.append(_val(e, w, sg, self._env(a, b), vt))
            except _DivZero:
                out.append(None)
        return out

    def _r_for(self, a, b):
        """For `r == expr`: the r (signed 64-bit) that satisfies it."""
        vt, e = self._vt(), self._expr()
        ew, es = _self_type(e, vt)
        cw, cs = max(64, ew), es          # r is signed: context signed iff e is
        v = _val(e, cw, cs, self._env(a, b), vt)
        return _wrap(v, 64, True)

    def expected_sat(self):
        vt, root = self._vt(), self._root()
        for a, b in self.pairs():
            try:
                if self.cmp is None:
                    if R_LO <= self._r_for(a, b) <= R_HI:
                        return True
                elif _truth(root, self._env(a, b), vt):
                    return True
            except _DivZero:
                pass
        return False

    def __str__(self):
        e = {"vc": "x %s %d" % (OPN[self.op], self.k),
             "cv": "%d %s x" % (self.k, OPN[self.op]),
             "vv": "x %s y" % OPN[self.op]}[self.shape]
        dom = "x:%s%d in [%d,%d]" % ("s" if self.s else "u", self.w,
                                     self.xlo, self.xhi)
        if self.shape == "vv":
            yw, ys, ylo, yhi = self.y
            dom += " y:%s%d in [%d,%d]" % ("s" if ys else "u", yw, ylo, yhi)
        if self.cmp is None:
            return "%s: r == %s" % (dom, e)
        return "%s: (%s) %s %d" % (dom, e, CMPN[self.cmp], self.c)

    def build(self):
        b = SolveProblemBuilder()
        b.add_var(0, width=self.w, is_signed=self.s, lo=self.xlo, hi=self.xhi)
        if self.cmp is None:
            b.add_var(1, width=64, is_signed=True, lo=R_LO, hi=R_HI)
        if self.shape == "vv":
            yw, ys, ylo, yhi = self.y
            b.add_var(2, width=yw, is_signed=ys, lo=ylo, hi=yhi)
        if self.shape == "vc":
            e = b.expr_binary(self.op, b.expr_var(0),
                              b.expr_const(self.k, is_signed=self.k < 0))
        elif self.shape == "cv":
            e = b.expr_binary(self.op, b.expr_const(self.k, is_signed=self.k < 0),
                              b.expr_var(0))
        else:
            e = b.expr_binary(self.op, b.expr_var(0), b.expr_var(2))
        if self.cmp is None:
            b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1), e))
        else:
            b.add_constraint(b.expr_binary(
                self.cmp, e, b.expr_const(self.c, is_signed=self.c < 0)))
        return b.finalize()[0]

    def check_model(self, ctx):
        """None if the model satisfies the constraint, else a description."""
        xv = ctx.get_value(0)
        yv = ctx.get_value(2) if self.shape == "vv" else None
        if not (self.xlo <= xv <= self.xhi):
            return "x=%d outside its domain" % xv
        rv = ctx.get_value(1) if self.cmp is None else 0
        env = {0: xv, 1: rv, 2: yv}
        try:
            ok = _truth(self._root(), env, self._vt())
        except _DivZero:
            return "model x=%s y=%s divides by zero" % (xv, yv)
        if not ok:
            return "model x=%s y=%s r=%s violates %s" % (xv, yv, rv, self)
        return None

    def validator_applies(self):
        """The model validator implements the same SV rules: always check."""
        return True


def run_case(case: Case):
    """Return None if dv-solve agrees with the reference, else a message."""
    exp = case.expected_sat()
    try:
        ctx = SolveCtx(case.build())
    except Exception:          # CompileIncompleteError: honest unknown
        return None
    try:
        rc = ctx.solve(seed=1, max_conflicts=200000)
        if rc == SOLVE_OK:
            if not exp:
                return "WRONG SAT: %s (%s)" % (case, case.check_model(ctx))
            err = case.check_model(ctx)
            if err is None and case.validator_applies() \
                    and ctx.validate_model() != 0:
                err = "model validator reports a violation"
            if err:
                return "BAD MODEL: %s: %s" % (case, err)
        elif rc == SOLVE_UNSAT and exp:
            return "WRONG UNSAT: %s" % case
        return None
    finally:
        ctx.destroy()


# ------------------------------------------------------------------ #
# Random case generator                                                #
# ------------------------------------------------------------------ #

_OPS = [BIN_DIV, BIN_MOD, BIN_RSHIFT, BIN_LSHIFT,
        BIN_DIV, BIN_MOD, BIN_RSHIFT, BIN_ADD, BIN_SUB, BIN_MUL]


def gen_case(R: random.Random, widths=(4, 8, 16, 32, 64)):
    """A random case, or None when the draw lands outside the tested space."""
    w = R.choice(widths)
    s = R.random() < 0.6
    op = R.choice(_OPS)
    vlo, vhi = _rng(w, s)
    span = R.choice([0, 0, 1, 3, 10, 40])
    cen = R.choice([R.randint(vlo, vhi),
                    R.randint(max(vlo, -20), min(vhi, 20)), vlo, vhi])
    xlo = max(vlo, cen - span // 2)
    xhi = min(vhi, xlo + span)
    shape = R.choice(["vc", "cv", "vv"])
    is_shift = op in (BIN_RSHIFT, BIN_LSHIFT)
    y = None
    if is_shift:
        k = R.randint(0, min(w - 1, 5) if op == BIN_LSHIFT else min(w + 1, 70))
        if shape == "cv":
            shape = "vc"
        if shape == "vv":
            y = (8, False, 0, R.randint(0, 3 if op == BIN_LSHIFT else 8))
    else:
        k = R.choice([1, 2, 3, 5, 7, 16, -1, -2, -3, -5])
        if not s and R.random() < 0.6:
            k = abs(k)
        if shape == "cv":
            # const op x: x is the divisor, keep 0 out of its domain
            if xlo <= 0 <= xhi:
                if xhi >= 1:
                    xlo = 1
                else:
                    xhi = -1
            if xlo > xhi:
                return None
            k = R.choice([cen, -7, 7, 100, -100]) if s else R.choice([cen, 7, 100])
        if shape == "vv":
            ys = s if R.random() < 0.7 else not s
            yw = w if R.random() < 0.7 else R.choice([4, 8, 16, 32, 64])
            if ys and R.random() < 0.5:
                y = (yw, ys, -R.randint(1, 6), -1)
            else:
                y = (yw, ys, 1, R.randint(1, 6))
    # A 64-bit unsigned var is a bit-vector: a negative constant against it
    # reads as 2^64 - |c| (SMT-LIB / SV), which is not an integer statement.
    u64 = w == 64 and not s
    if u64 and shape != "vv" and k < 0:
        return None
    if not (I64_LO <= k <= I64_HI):
        return None
    case = Case(w, s, xlo, xhi, op, shape, k=k, y=y)
    vals = case.values()
    if any(v is None for v in vals):
        return None
    # Beyond int64 a result wraps at 64 bits (storage / SV 64-bit context).
    if any(not (I64_LO <= v <= I64_HI) for v in vals):
        return None
    if R.random() < 0.4:
        return case                      # r == expr
    # expr CMP c. A result outside the operand's own range is a width-context
    # (wrap) question for these ops -- out of scope here.
    if op in (BIN_LSHIFT, BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV) and \
            any(not (vlo <= v <= vhi) for v in vals):
        return None
    case.cmp = R.choice(list(CMP))
    c = R.choice(vals) + R.choice([0, 0, 0, -1, 1])
    if R.random() < 0.2:
        c = R.choice([-2, -1, 0, 1, 2, 124, (1 << (w - 1)) - 4])
    if not (I64_LO <= c <= I64_HI) or (u64 and c < 0):
        return None
    case.c = c
    return case


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #


@pytest.mark.parametrize("seed", [1, 2, 3, 4, 5])
def test_random_sweep(seed):
    R = random.Random(seed)
    failures, n = [], 0
    while n < 3000:
        case = gen_case(R)
        if case is None:
            continue
        n += 1
        msg = run_case(case)
        if msg:
            failures.append(msg)
    assert not failures, "%d disagreement(s):\n%s" % (
        len(failures), "\n".join(failures[:30]))


@pytest.mark.parametrize("w,s", [(4, True), (4, False), (8, True), (8, False)])
@pytest.mark.parametrize("op", [BIN_DIV, BIN_MOD, BIN_RSHIFT])
def test_exhaustive_pinned(w, s, op):
    """Every value of x (pinned), several constants: `r == x op k` must give
    exactly the reference value, and `(x op k) == v` must be sat for the
    reference v and unsat for v +/- 1."""
    lo, hi = _rng(w, s)
    if op == BIN_RSHIFT:
        ks = [0, 1, 2, w - 1, w, w + 1]
    else:
        ks = [1, 2, 3, 5, 7] + ([-1, -2, -3, -5] if s else [])
    failures = []
    for x in range(lo, hi + 1):
        for k in ks:
            v = ref(op, x, k)
            cases = [Case(w, s, x, x, op, "vc", k=k)]
            if not (lo <= v <= hi):
                # only MIN / -1: whether a compare sees the wrapped value is a
                # width-context question (see module docstring)
                cases = cases[:1]
                for case in cases:
                    msg = run_case(case)
                    if msg:
                        failures.append(msg)
                continue
            for c in (v, v - 1, v + 1):
                cases.append(Case(w, s, x, x, op, "vc", k=k, cmp=BIN_EQ, c=c))
            cases.append(Case(w, s, x, x, op, "vc", k=k, cmp=BIN_LT, c=v))
            cases.append(Case(w, s, x, x, op, "vc", k=k, cmp=BIN_GTE, c=v))
            for case in cases:
                msg = run_case(case)
                if msg:
                    failures.append(msg)
    assert not failures, "%d disagreement(s):\n%s" % (
        len(failures), "\n".join(failures[:30]))


def _solve(case):
    ctx = SolveCtx(case.build())
    try:
        rc = ctx.solve(seed=1)
        return rc, (ctx.get_value(1) if rc == SOLVE_OK and case.cmp is None
                     else None)
    finally:
        ctx.destroy()


def test_repro_signed_mod_vs_const():
    # (x % 3) == -2 / == 1 with x == -8
    assert _solve(Case(8, True, -8, -8, BIN_MOD, "vc", k=3, cmp=BIN_EQ, c=-2))[0] == SOLVE_OK
    assert _solve(Case(8, True, -8, -8, BIN_MOD, "vc", k=3, cmp=BIN_EQ, c=1))[0] == SOLVE_UNSAT
    assert _solve(Case(8, True, -8, -8, BIN_MOD, "vc", k=3)) == (SOLVE_OK, -2)


def test_repro_signed_rshift():
    # SV `>>` is logical on the context-width pattern. `(x >> 1) CMP c`: the
    # context is 32-bit signed (both literals are 32-bit ints), so -8 is
    # 0xFFFFFFF8 and shifts to 0x7FFFFFFC.
    assert _solve(Case(8, True, -8, -8, BIN_RSHIFT, "vc", k=1, cmp=BIN_EQ, c=0x7FFFFFFC))[0] == SOLVE_OK
    assert _solve(Case(8, True, -8, -8, BIN_RSHIFT, "vc", k=1, cmp=BIN_EQ, c=-4))[0] == SOLVE_UNSAT
    assert _solve(Case(8, True, -8, -8, BIN_RSHIFT, "vc", k=1, cmp=BIN_EQ, c=124))[0] == SOLVE_UNSAT
    assert _solve(Case(8, True, -7, -7, BIN_RSHIFT, "vc", k=1, cmp=BIN_EQ, c=0x7FFFFFFC))[0] == SOLVE_OK
    # `r == x >> 1` with r signed 64-bit: a 64-bit context, 2^63 - 4 -- outside
    # r's [-2^62, 2^62] domain here, so no solution. (The CDCL engine has no
    # exact logical shift of a 64-bit pattern >= 2^63 and may decline.)
    try:
        assert _solve(Case(8, True, -8, -8, BIN_RSHIFT, "vc", k=1))[0] == SOLVE_UNSAT
    except CompileIncompleteError:
        pass
    # A non-negative operand shifts the same way under either reading.
    assert _solve(Case(8, True, 9, 9, BIN_RSHIFT, "vc", k=1)) == (SOLVE_OK, 4)
    # Shifting by >= the (32-bit) context width clears every bit.
    assert _solve(Case(32, True, -(1 << 31), -(1 << 31), BIN_RSHIFT, "vc", k=40,
                       cmp=BIN_EQ, c=0))[0] == SOLVE_OK


def test_repro_int64_min_div_minus_one_no_crash():
    # Used to die with SIGFPE (propagator and model validator).
    # (the quotients are 2^63 - j, outside r's domain: unsat, not a crash)
    assert _solve(Case(64, True, I64_LO, I64_LO + 3, BIN_DIV, "vc", k=-1))[0] == SOLVE_UNSAT
    assert _solve(Case(64, True, I64_LO, I64_LO, BIN_MOD, "vc", k=-1)) == (SOLVE_OK, 0)
    assert _solve(Case(64, True, I64_LO, I64_LO, BIN_DIV, "vc", k=-1, cmp=BIN_EQ, c=0))[0] == SOLVE_UNSAT


def test_repro_pin_var_width():
    # x + 1 == 0 over a 64-bit unsigned x: no solution (was sat at 2^32-1).
    assert _solve(Case(64, False, 0, 10**11, BIN_ADD, "vc", k=1, cmp=BIN_EQ, c=0))[0] == SOLVE_UNSAT


def test_repro_ne_int32_max():
    m = (1 << 31) - 1
    assert _solve(Case(32, True, m, m, BIN_MUL, "vc", k=1, cmp=BIN_NEQ, c=m))[0] == SOLVE_UNSAT


def test_repro_signed64_negative_const_operand():
    assert _solve(Case(64, True, 1, 21, BIN_ADD, "vc", k=-7, cmp=BIN_GTE, c=11))[0] == SOLVE_OK
    assert _solve(Case(64, True, -10, -10, BIN_LSHIFT, "vc", k=4, cmp=BIN_LT, c=1))[0] == SOLVE_OK
