"""Differential test: builder-API expression sizing and signedness follow
IEEE 1800 (SystemVerilog) rules, in every engine.

The reference below is a small pure-Python SystemVerilog expression evaluator.
Every case is a constraint over a few variables with small (or pinned)
domains, so the full set of satisfying assignments is computed by
enumeration. Each engine must agree with it:

* CDCL (``SolveCtx``): a SAT model must satisfy the reference and the model
  validator must accept it; UNSAT must mean "no assignment satisfies it".
  Declining (``CompileIncompleteError``) or a timeout is allowed -- that is the
  engine being honest, not wrong.
* Bit-blast (``BVSatCtx``): the same, with ``BVSAT_UNKNOWN`` allowed.
* Model validator: evaluated directly on pinned assignments, it must report a
  violation exactly when the reference says the constraint is false.

Semantics (what the reference implements)
-----------------------------------------
* Self-determined type of a node, ``(width, signed)``:
  - variable: its declared width / signedness;
  - unsized constant (``expr_const(v, is_signed)``): a SystemVerilog integer
    literal -- ``(32, signed)`` when ``v`` fits in int32; else ``(32, unsigned)``
    when ``is_signed`` is false and ``v`` fits in uint32; else 64 bits, signed
    when ``v`` is negative or ``is_signed`` is set, unsigned otherwise;
  - sized constant (``expr_const(v, is_signed, width)``): ``(width, is_signed)``;
  - ``+ - * / % & | ^``: ``(max width, both signed)``;
  - ``<< >>``: the left operand's type (the shift amount is self-determined);
  - comparisons, ``&&``, ``||``, ``!``: ``(1, unsigned)``;
  - unary ``-`` / ``~``: the operand's type; ``?:``: like a binary operator
    over the two branches.
* A comparison is evaluated in the context ``(max width of both sides, both
  sides signed)``; that context propagates down through the
  context-determined operators. Each operand is first sign- or zero-extended
  per ITS OWN signedness, then the operation is done at the context width and
  read back per the context signedness (2's complement wrap).
* ``/`` and ``%`` truncate toward zero when signed. ``>>`` is a LOGICAL shift of
  the context-width bit pattern (SystemVerilog ``>>``). ``>>>`` (BIN_ASHR) is
  an ARITHMETIC shift of it in a signed context (sign bit replicated; a shift
  by >= the width gives 0 or -1) and the same as ``>>`` in an unsigned one. A
  shift amount is the unsigned value of its own bit pattern.
"""
from __future__ import annotations

import ctypes
import inspect
import os
import itertools
import random

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.bvsat import BVSatCtx, BVSAT_SAT, BVSAT_UNSAT
from dv_solve.ctx import (
    SolveCtx, SOLVE_OK, SOLVE_UNSAT, CompileIncompleteError, CompileUnsatError,
)
from dv_solve.problem import (
    BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV, BIN_MOD, BIN_BAND, BIN_BOR, BIN_BXOR,
    BIN_LSHIFT, BIN_RSHIFT, BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE, BIN_GT, BIN_GTE,
    BIN_AND, BIN_OR, BIN_ASHR, UN_NEG, UN_NOT, UN_INVERT,
)

# ------------------------------------------------------------------ #
# Reference: a tiny SystemVerilog expression evaluator                #
# ------------------------------------------------------------------ #

ARITH = (BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV, BIN_MOD, BIN_BAND, BIN_BOR,
         BIN_BXOR)
SHIFTS = (BIN_LSHIFT, BIN_RSHIFT, BIN_ASHR)
CMPS = (BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE, BIN_GT, BIN_GTE)
OPN = {BIN_ADD: "+", BIN_SUB: "-", BIN_MUL: "*", BIN_DIV: "/", BIN_MOD: "%",
       BIN_BAND: "&", BIN_BOR: "|", BIN_BXOR: "^", BIN_LSHIFT: "<<",
       BIN_RSHIFT: ">>", BIN_EQ: "==", BIN_NEQ: "!=", BIN_LT: "<",
       BIN_LTE: "<=", BIN_GT: ">", BIN_GTE: ">=", BIN_AND: "&&",
       BIN_OR: "||", BIN_ASHR: ">>>"}

I32_LO, I32_HI = -(1 << 31), (1 << 31) - 1
I64_LO, I64_HI = -(1 << 63), (1 << 63) - 1


class DivZero(Exception):
    pass


class Var:
    def __init__(self, vid):
        self.vid = vid

    def __str__(self):
        return "v%d" % self.vid


class Const:
    def __init__(self, value, is_signed=False, width=0):
        self.value, self.is_signed, self.width = value, is_signed, width

    def __str__(self):
        if self.width:
            return "%d'%s%d" % (self.width, "s" if self.is_signed else "",
                                self.value)
        return "%d%s" % (self.value, "s" if self.is_signed else "")


class Bin:
    def __init__(self, op, l, r):
        self.op, self.l, self.r = op, l, r

    def __str__(self):
        return "(%s %s %s)" % (self.l, OPN[self.op], self.r)


class Un:
    def __init__(self, op, x):
        self.op, self.x = op, x

    def __str__(self):
        return "%s%s" % ({UN_NEG: "-", UN_NOT: "!", UN_INVERT: "~"}[self.op],
                         self.x)


class Ite:
    def __init__(self, c, t, e):
        self.c, self.t, self.e = c, t, e

    def __str__(self):
        return "(%s ? %s : %s)" % (self.c, self.t, self.e)


def wrap(v, w, s):
    v &= (1 << w) - 1
    if s and v >> (w - 1):
        v -= 1 << w
    return v


def const_type(c):
    if c.width:
        return c.width, bool(c.is_signed)
    v = c.value
    if I32_LO <= v <= I32_HI:
        return 32, True
    if not c.is_signed and 0 <= v <= 0xFFFFFFFF:
        return 32, False
    if v < 0:
        return 64, True
    return 64, bool(c.is_signed)


def const_own(c):
    w, s = const_type(c)
    return wrap(c.value, w, s)


def self_type(n, vt):
    if isinstance(n, Var):
        return vt[n.vid]
    if isinstance(n, Const):
        return const_type(n)
    if isinstance(n, Bin):
        if n.op in ARITH:
            lw, ls = self_type(n.l, vt)
            rw, rs = self_type(n.r, vt)
            return max(lw, rw), ls and rs
        if n.op in SHIFTS:
            return self_type(n.l, vt)
        return 1, False
    if isinstance(n, Un):
        if n.op == UN_NOT:
            return 1, False
        return self_type(n.x, vt)
    if isinstance(n, Ite):
        tw, ts = self_type(n.t, vt)
        ew, es = self_type(n.e, vt)
        return max(tw, ew), ts and es
    raise AssertionError(n)


def _tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def val(n, W, S, env, vt):
    """Value of `n` evaluated in context (W, S)."""
    if isinstance(n, Var):
        return wrap(env[n.vid], W, S)
    if isinstance(n, Const):
        return wrap(const_own(n), W, S)
    if isinstance(n, Bin):
        op = n.op
        if op in ARITH:
            a = val(n.l, W, S, env, vt)
            b = val(n.r, W, S, env, vt)
            if op == BIN_ADD:
                r = a + b
            elif op == BIN_SUB:
                r = a - b
            elif op == BIN_MUL:
                r = a * b
            elif op == BIN_BAND:
                r = a & b
            elif op == BIN_BOR:
                r = a | b
            elif op == BIN_BXOR:
                r = a ^ b
            elif op == BIN_DIV:
                if b == 0:
                    raise DivZero()
                r = _tdiv(a, b) if S else a // b
            else:
                if b == 0:
                    raise DivZero()
                r = a - b * _tdiv(a, b) if S else a % b
            return wrap(r, W, S)
        if op in SHIFTS:
            a = val(n.l, W, S, env, vt)
            rw, rs = self_type(n.r, vt)
            k = wrap(val(n.r, rw, rs, env, vt), rw, False)
            if op == BIN_ASHR and S:
                # `>>>` in a signed context: arithmetic shift of the signed
                # W-bit value (Python >> on a negative int floors).
                return a >> min(k, W)
            if k >= W:
                return 0
            if op == BIN_LSHIFT:
                return wrap(a << k, W, S)
            return wrap((a & ((1 << W) - 1)) >> k, W, S)
        if op in CMPS:
            lw, ls = self_type(n.l, vt)
            rw, rs = self_type(n.r, vt)
            cw, cs = max(lw, rw), ls and rs
            a = val(n.l, cw, cs, env, vt)
            b = val(n.r, cw, cs, env, vt)
            t = {BIN_EQ: a == b, BIN_NEQ: a != b, BIN_LT: a < b,
                 BIN_LTE: a <= b, BIN_GT: a > b, BIN_GTE: a >= b}[op]
            return wrap(int(t), W, S)
        if op == BIN_AND:
            return int(truth(n.l, env, vt) and truth(n.r, env, vt))
        if op == BIN_OR:
            return int(truth(n.l, env, vt) or truth(n.r, env, vt))
        raise AssertionError(op)
    if isinstance(n, Un):
        if n.op == UN_NOT:
            return int(not truth(n.x, env, vt))
        a = val(n.x, W, S, env, vt)
        return wrap(-a if n.op == UN_NEG else ~a, W, S)
    if isinstance(n, Ite):
        return val(n.t if truth(n.c, env, vt) else n.e, W, S, env, vt)
    raise AssertionError(n)


def truth(n, env, vt):
    w, s = self_type(n, vt)
    return val(n, w, s, env, vt) != 0


# ------------------------------------------------------------------ #
# Cases                                                                #
# ------------------------------------------------------------------ #

_HAS_SIZED = "width" in inspect.signature(SolveProblemBuilder.expr_const).parameters


def _full(w, s):
    if s:
        return -(1 << (w - 1)), (1 << (w - 1)) - 1
    # An unsigned 64-bit variable is declared through the int64 API, so it
    # cannot reach 2^63; and hi == INT64_MAX is the bit-blaster's "whole
    # 64-bit range" sentinel, which the CDCL engine does not share. Stay
    # below it so both engines see the same domain.
    return 0, min((1 << w) - 1, I64_HI - 1)


class Case:
    """`vars`: list of (width, signed, lo, hi); `root`: a Boolean AST."""

    def __init__(self, vars_, root, nonzero=()):
        self.vars = vars_
        self.root = root
        self.vt = {i: (w, s) for i, (w, s, _, _) in enumerate(vars_)}
        self.nonzero = set(nonzero)   # vars that must not take the value 0

    def __str__(self):
        doms = " ".join("v%d:%s%d[%d,%d]" % (i, "s" if s else "u", w, lo, hi)
                        for i, (w, s, lo, hi) in enumerate(self.vars))
        return "%s  %s" % (doms, self.root)

    def domains(self):
        out = []
        for i, (_, _, lo, hi) in enumerate(self.vars):
            vals = range(lo, hi + 1)
            if i in self.nonzero:
                vals = [v for v in vals if v != 0]
            out.append(list(vals))
        return out

    def holds(self, env):
        try:
            return truth(self.root, env, self.vt)
        except DivZero:
            return False

    def solutions(self):
        return [env for env in itertools.product(*self.domains())
                if self.holds(env)]

    # -- builder ---------------------------------------------------- #

    def _emit(self, b, n):
        if isinstance(n, Var):
            return b.expr_var(n.vid)
        if isinstance(n, Const):
            if n.width:
                return b.expr_const(n.value, is_signed=n.is_signed,
                                    width=n.width)
            return b.expr_const(n.value, is_signed=n.is_signed)
        if isinstance(n, Bin):
            return b.expr_binary(n.op, self._emit(b, n.l), self._emit(b, n.r))
        if isinstance(n, Un):
            return b.expr_unary(n.op, self._emit(b, n.x))
        if isinstance(n, Ite):
            return b.expr_ite(self._emit(b, n.c), self._emit(b, n.t),
                              self._emit(b, n.e))
        raise AssertionError(n)

    def build(self, pin=None, with_constraint=True):
        b = SolveProblemBuilder()
        for i, (w, s, lo, hi) in enumerate(self.vars):
            if pin is not None:
                lo = hi = pin[i]
            b.add_var(i, width=w, is_signed=s, lo=lo, hi=hi)
        if with_constraint:
            b.add_constraint(self._emit(b, self.root))
            for i in self.nonzero:
                b.add_constraint(b.expr_binary(BIN_NEQ, b.expr_var(i),
                                               b.expr_const(0)))
        return b.finalize()[0]


# ------------------------------------------------------------------ #
# Engines                                                              #
# ------------------------------------------------------------------ #

# Honest non-answers, tallied for the curious (not asserted).
STATS = {"cases": 0, "cdcl_declined": 0, "cdcl_timeout": 0, "bb_unknown": 0}


def check_cdcl(case, sols):
    """None if the CDCL engine agrees with the reference, else a message."""
    exp = bool(sols)
    STATS["cases"] += 1
    try:
        ctx = SolveCtx(case.build())
    except CompileUnsatError:
        return "cdcl WRONG UNSAT (compile): %s" % case if exp else None
    except CompileIncompleteError:
        STATS["cdcl_declined"] += 1
        return None
    try:
        rc = ctx.solve(seed=1, max_conflicts=100000)
        if rc == SOLVE_OK:
            env = tuple(ctx.get_value(i) for i in range(len(case.vars)))
            if not exp:
                return "cdcl WRONG SAT: %s model=%s" % (case, env)
            if not case.holds(env) or any(
                    not (lo <= v <= hi) for v, (_, _, lo, hi)
                    in zip(env, case.vars)):
                return "cdcl BAD MODEL: %s model=%s" % (case, env)
            if ctx.validate_model() != 0:
                return "cdcl model %s rejected by validator: %s" % (env, case)
        elif rc == SOLVE_UNSAT and exp:
            return "cdcl WRONG UNSAT: %s (e.g. %s)" % (case, sols[0])
        elif rc not in (SOLVE_OK, SOLVE_UNSAT):
            STATS["cdcl_timeout"] += 1
        return None
    finally:
        ctx.destroy()


# The bit-blaster has no wall-clock budget of its own; a few 64-bit
# multiply/divide circuits can be slow for the SAT solver, so cap the search.
# Running out of budget is BVSAT_UNKNOWN -- an allowed answer.
_BB_CONFLICTS = "50000"


def check_bb(case, sols):
    exp = bool(sols)
    old = os.environ.get("DV_BB_MAX_CONFLICTS")
    os.environ["DV_BB_MAX_CONFLICTS"] = _BB_CONFLICTS
    try:
        with BVSatCtx(case.build()) as bb:
            return _check_bb(case, sols, exp, bb)
    finally:
        if old is None:
            del os.environ["DV_BB_MAX_CONFLICTS"]
        else:
            os.environ["DV_BB_MAX_CONFLICTS"] = old


def _check_bb(case, sols, exp, bb):
    # seed 0: no post-solve diversification pass, which is slow on the
    # large 64-bit divider circuits and irrelevant to correctness.
    rc = bb.check(seed=0)
    if rc == BVSAT_SAT:
        env = tuple(bb.value(i) for i in range(len(case.vars)))
        if not exp:
            return "bb WRONG SAT: %s model=%s" % (case, env)
        if not case.holds(env):
            return "bb BAD MODEL: %s model=%s" % (case, env)
    elif rc == BVSAT_UNSAT and exp:
        return "bb WRONG UNSAT: %s (e.g. %s)" % (case, sols[0])
    elif rc not in (BVSAT_SAT, BVSAT_UNSAT):
        STATS["bb_unknown"] += 1
    return None


def check_validator(case, env):
    """Pin the vars to `env`, then ask the validator about the constraint."""
    try:
        ctx = SolveCtx(case.build(pin=env, with_constraint=False))
    except (CompileIncompleteError, CompileUnsatError):
        return None
    try:
        if ctx.solve(seed=1) != SOLVE_OK:
            return None
        target = case.build()
        n = ctx._lib.solver_validate_model(
            ctx._ctx, ctypes.cast(target, ctypes.c_void_p).value, None)
        exp = case.holds(env)
        if (n == 0) != exp:
            return "validator says %s for %s at %s (reference: %s)" % (
                "ok" if n == 0 else "violated", case, env, exp)
        return None
    finally:
        ctx.destroy()


def run_case(case, validator_samples=2, R=None):
    sols = case.solutions()
    out = []
    for fn in (check_cdcl, check_bb):
        msg = fn(case, sols)
        if msg:
            out.append(msg)
    if validator_samples and R is not None:
        allenv = list(itertools.product(*case.domains()))
        picks = R.sample(allenv, min(validator_samples, len(allenv)))
        if sols:
            picks.append(R.choice(sols))
        for env in picks:
            msg = check_validator(case, env)
            if msg:
                out.append(msg)
    return out


# ------------------------------------------------------------------ #
# Generators                                                           #
# ------------------------------------------------------------------ #

WIDTHS = (4, 8, 16, 32, 64)


def window(R, w, s, size):
    """A small domain inside the full range of a (w, s) variable, around an
    interesting point."""
    lo, hi = _full(w, s)
    if hi - lo + 1 <= size:
        return lo, hi
    pts = [0, lo, hi, R.randint(lo, hi)]
    if not s:
        pts.append(1 << (w - 1))
    else:
        pts.append(-1)
    if w > 32:
        pts += [1 << 31, (1 << 32) - 1] if not s else [I32_LO, I32_HI]
    c = R.choice(pts)
    a = max(lo, c - size // 2)
    b = min(hi, a + size - 1)
    a = max(lo, b - size + 1)
    return a, b


def interesting_consts(R, w, s):
    """Constants in-width, out-of-width, negative and > 32-bit."""
    lo, hi = _full(w, s)
    cs = [0, 1, 3, 7, hi, lo, R.randint(lo, hi), -1, -3, -200, 200, 300,
          (1 << w) + 3 if w < 62 else 5, 0x80000000, 0xFFFFFFFF,
          (1 << 33) + 5, -(1 << 35), (1 << 40) + 1]
    return cs


def make_const(R, cands, sized_ok=True):
    v = R.choice(cands)
    if not (I64_LO <= v <= I64_HI):
        v = 5
    r = R.random()
    if _HAS_SIZED and sized_ok and r < 0.12:
        w = R.choice([4, 8, 16, 32, 64])
        return Const(wrap(v, w, False), is_signed=R.random() < 0.5, width=w)
    if r < 0.25:
        return Const(v, is_signed=True)
    return Const(v)


def gen_case(R, single_var_exhaustive=False):
    w = R.choice(WIDTHS if not single_var_exhaustive else (4, 8))
    s = R.random() < 0.5
    op = R.choice(ARITH + SHIFTS + (None,))
    shape = R.choice(["vc", "cv", "vv", "vc_v", "vv_v"])
    cmp = R.choice(CMPS)
    vars_ = []
    nonzero = []

    def add_var(w_, s_, size):
        lo, hi = window(R, w_, s_, size)
        vars_.append((w_, s_, lo, hi))
        return Var(len(vars_) - 1)

    if single_var_exhaustive or shape in ("vc", "cv"):
        size = 256 if w <= 8 else 24
    else:
        size = 16 if w <= 4 else 12
    x = add_var(w, s, size)
    kcands = interesting_consts(R, w, s)
    if op is None:
        # plain comparison: x CMP c, or x CMP y (mixed widths/signedness)
        if shape in ("vv", "vv_v", "vc_v"):
            y = add_var(R.choice(WIDTHS), R.random() < 0.5, 16)
            root = Bin(cmp, x, y) if R.random() < 0.5 else Bin(cmp, y, x)
        else:
            c = make_const(R, kcands)
            root = Bin(cmp, x, c) if R.random() < 0.5 else Bin(cmp, c, x)
        return Case(vars_, root)

    if op in SHIFTS:
        shamts = [0, 1, 3, w - 1, w, w + 1, 31, 32, 33, 63, 64, 70]
        if shape in ("vv", "vv_v"):
            # A variable amount: various widths and signedness, small values,
            # values around the width, and (for wide amounts) huge ones --
            # a negative signed amount reads as a huge unsigned one.
            yw = R.choice([8, 8, 16, 64])
            ys = R.random() < 0.3
            lo = R.choice([0, 0, max(0, w - 4), 60])
            if ys and R.random() < 0.4:
                lo = -2
            if yw == 64 and R.random() < 0.2:
                lo = R.choice([1 << 40, (1 << 63) - 20])
            hi = lo + 11
            lo_ok, hi_ok = _full(yw, ys)
            lo, hi = max(lo, lo_ok), min(hi, hi_ok)
            vars_.append((yw, ys, lo, hi))
            y = Var(len(vars_) - 1)
            e = Bin(op, x, y)
        else:
            e = Bin(op, x, Const(R.choice(shamts)))
    else:
        if shape == "cv":
            k = make_const(R, kcands)
            e = Bin(op, k, x)
            if op in (BIN_DIV, BIN_MOD):
                nonzero.append(x.vid)
        elif shape in ("vv", "vv_v"):
            yw = w if R.random() < 0.6 else R.choice(WIDTHS)
            ys = s if R.random() < 0.6 else (not s)
            y = add_var(yw, ys, 12)
            e = Bin(op, x, y)
            if op in (BIN_DIV, BIN_MOD):
                nonzero.append(y.vid)
        else:
            k = make_const(R, kcands)
            if op in (BIN_DIV, BIN_MOD) and k.value == 0:
                k = Const(3)
            e = Bin(op, x, k)
    if shape in ("vc_v", "vv_v"):
        z = add_var(R.choice(WIDTHS), R.random() < 0.5, 12)
        root = Bin(cmp, e, z) if R.random() < 0.7 else Bin(cmp, z, e)
    else:
        c = make_const(R, kcands + [0, 1, 2, 4, 6])
        root = Bin(cmp, e, c) if R.random() < 0.8 else Bin(cmp, c, e)
    return Case(vars_, root, nonzero)


def _sweep(seed, n, exhaustive=False):
    R = random.Random(seed)
    failures = []
    for _ in range(n):
        case = gen_case(R, single_var_exhaustive=exhaustive)
        failures += run_case(case, validator_samples=1, R=R)
    return failures


def _report(failures):
    return "%d disagreement(s):\n%s" % (len(failures), "\n".join(failures[:40]))


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

@pytest.mark.parametrize("seed", range(4))
def test_exhaustive_narrow(seed):
    """Single var of width 4/8 over its whole domain."""
    failures = _sweep(1000 + seed, 250, exhaustive=True)
    assert not failures, _report(failures)


@pytest.mark.parametrize("seed", range(6))
def test_random_sweep(seed):
    """All shapes, widths 4..64, pinned domains for the wide ones."""
    failures = _sweep(seed, 300)
    assert not failures, _report(failures)


def _u(w, lo=None, hi=None):
    a, b = _full(w, False)
    return (w, False, a if lo is None else lo, b if hi is None else hi)


def _s(w, lo=None, hi=None):
    a, b = _full(w, True)
    return (w, True, a if lo is None else lo, b if hi is None else hi)


def _expect(case, sat):
    sols = case.solutions()
    assert bool(sols) == sat, "reference disagrees with the expectation"
    failures = []
    for fn in (check_cdcl, check_bb):
        msg = fn(case, sols)
        if msg:
            failures.append(msg)
    assert not failures, _report(failures)
    return sols


def test_example_add_eq_zero_u4():
    # x + 3 == 0 over unsigned 4-bit x: the context is 32 bits (3 and 0 are
    # 32-bit integer literals), so x + 3 is in [3, 18] -- never 0.
    _expect(Case([_u(4)], Bin(BIN_EQ, Bin(BIN_ADD, Var(0), Const(3)),
                                Const(0))), sat=False)


def test_example_add_lt_u8_no_wrap():
    # x + 3 < 10 over unsigned 8-bit x: x in [0, 6]; 254 (257 wraps to 1 at
    # 8 bits) is NOT a solution because the context is 32 bits.
    case = Case([_u(8)], Bin(BIN_LT, Bin(BIN_ADD, Var(0), Const(3)), Const(10)))
    sols = _expect(case, sat=True)
    assert sorted(e[0] for e in sols) == list(range(7))


def test_example_add_gt_out_of_width_const():
    # x + 200 > 290: x > 90. Used to raise CompileUnsatError.
    case = Case([_u(8)], Bin(BIN_GT, Bin(BIN_ADD, Var(0), Const(200)),
                             Const(290)))
    sols = _expect(case, sat=True)
    assert sorted(e[0] for e in sols) == list(range(91, 256))


def test_example_mul_gt_out_of_width_const():
    # x * 3 > 300: x >= 101. Used to raise CompileUnsatError.
    case = Case([_u(8)], Bin(BIN_GT, Bin(BIN_MUL, Var(0), Const(3)),
                             Const(300)))
    sols = _expect(case, sat=True)
    assert sorted(e[0] for e in sols) == list(range(101, 256))


def test_example_same_width_vars_are_modular():
    # r == a + b with r, a, b all u8: the context is 8 bits, so it wraps.
    case = Case([_u(8, 44, 44), _u(8, 200, 200), _u(8, 100, 100)],
                Bin(BIN_EQ, Var(0), Bin(BIN_ADD, Var(1), Var(2))))
    _expect(case, sat=True)


def test_example_unsigned_beats_signed():
    # signed x < unsigned y is an UNSIGNED compare: x == -1 is 0xFF.. > y.
    case = Case([_s(8, -1, -1), _u(8, 5, 5)], Bin(BIN_LT, Var(0), Var(1)))
    _expect(case, sat=False)
    # and against a negative constant an unsigned var is never smaller.
    _expect(Case([_u(8)], Bin(BIN_LT, Var(0), Const(-1))), sat=True)
    _expect(Case([_u(8)], Bin(BIN_GT, Var(0), Const(-1))), sat=False)


def test_example_signed_rshift_is_logical():
    # x >> 1 with x == -8 (signed 8-bit) in a signed 32-bit context: the
    # pattern 0xFFFFFFF8 shifted logically is 0x7FFFFFFC.
    case = Case([_s(8, -8, -8)], Bin(BIN_EQ, Bin(BIN_RSHIFT, Var(0), Const(1)),
                                     Const(0x7FFFFFFC)))
    _expect(case, sat=True)
    case = Case([_s(8, -8, -8)], Bin(BIN_EQ, Bin(BIN_RSHIFT, Var(0), Const(1)),
                                     Const(-4)))
    _expect(case, sat=False)


def test_example_signed_division_truncates():
    case = Case([_s(8, -8, -8)], Bin(BIN_EQ, Bin(BIN_DIV, Var(0), Const(3)),
                                     Const(-2)))
    _expect(case, sat=True)
    case = Case([_s(8, -8, -8)], Bin(BIN_EQ, Bin(BIN_MOD, Var(0), Const(3)),
                                     Const(-2)))
    _expect(case, sat=True)


def test_example_ashr_signed_is_arithmetic():
    # x >>> 1 with x == -8 (signed 8-bit): a signed 32-bit context, so the
    # sign is replicated: -4.
    case = Case([_s(8, -8, -8)], Bin(BIN_EQ, Bin(BIN_ASHR, Var(0), Const(1)),
                                     Const(-4)))
    _expect(case, sat=True)
    case = Case([_s(8, -8, -8)], Bin(BIN_EQ, Bin(BIN_ASHR, Var(0), Const(1)),
                                     Const(124)))
    _expect(case, sat=False)
    # shifting by >= the context width leaves only the sign: -1 / 0.
    case = Case([_s(8, -8, -8)], Bin(BIN_EQ, Bin(BIN_ASHR, Var(0), Const(40)),
                                     Const(-1)))
    _expect(case, sat=True)
    case = Case([_s(8, 5, 5)], Bin(BIN_EQ, Bin(BIN_ASHR, Var(0), Const(40)),
                                   Const(0)))
    _expect(case, sat=True)


def test_example_ashr_unsigned_is_logical():
    # unsigned 8-bit x == 0xF0: an unsigned context, so >>> is >>.
    case = Case([_u(8, 0xF0, 0xF0)],
                Bin(BIN_EQ, Bin(BIN_ASHR, Var(0), Const(4)), Const(0x0F)))
    _expect(case, sat=True)
    # an unsigned operand makes the whole context unsigned: logical too.
    case = Case([_s(8, -8, -8), _u(32, 1, 1)],
                Bin(BIN_EQ, Bin(BIN_ASHR, Bin(BIN_ADD, Var(0), Var(1)), Const(1)),
                    Const(0x7FFFFFFC)))
    _expect(case, sat=True)


@pytest.mark.skipif(not _HAS_SIZED, reason="sized constants not exposed")
def test_example_sized_constants_are_modular():
    # with 8-bit sized constants the context stays 8 bits and wraps:
    # x + 8'd3 == 8'd0 at x == 253.
    case = Case([_u(8)], Bin(BIN_EQ, Bin(BIN_ADD, Var(0), Const(3, width=8)),
                             Const(0, width=8)))
    sols = _expect(case, sat=True)
    assert [e[0] for e in sols] == [253]


if __name__ == "__main__":   # failure census
    import sys
    tot = []
    for seed in range(4):
        tot += _sweep(1000 + seed, 250, exhaustive=True)
    for seed in range(6):
        tot += _sweep(seed, 300)
    kinds = {}
    for f in tot:
        k = f.split(":")[0]
        k = " ".join(k.split()[:3])
        kinds[k] = kinds.get(k, 0) + 1
    print("total failures:", len(tot))
    for k, v in sorted(kinds.items(), key=lambda t: -t[1]):
        print("  %5d  %s" % (v, k))
    if "-v" in sys.argv:
        print("\n".join(tot[:80]))
