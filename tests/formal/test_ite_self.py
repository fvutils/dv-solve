"""A comparison with one side the same as a branch of an ite on the other.

`ite(c, x, y) < y` -- found by the wide campaign (B103) -- left the engine a
climb: with c open, r = ite(..) takes the hull of x and y, `r < y` raises y
above r's low end, the hull follows, one value a round, to the propagation
cap. The pinned solve was a timeout where `c && x < y` is microseconds.
Compile rewrites it (dvs_sv_rewrite_masks, every front end): the self branch
compares a value with itself, so `ite(c, a, b) op b` is `c && a op b` for an
irreflexive op and `!c || a op b` for a reflexive one; likewise through the
same zero or sign extension on both sides.

Checked exhaustively against the IR's evaluator (see
test_align_rewrite._check): every comparison, either branch, either side,
under extensions, with a condition on another variable or on the branches.
"""
from __future__ import annotations

import itertools

import pytest

from tests.formal.test_align_rewrite import _check

dv = pytest.importorskip("dv_solve")
from dv_solve.builder import SolveProblemBuilder            # noqa: E402
from dv_solve.ctx import SOLVE_OK, SOLVE_UNSAT, SolveCtx    # noqa: E402

W = 3
X, Y, Z = ("var", "x", W), ("var", "y", W), ("var", "z", 2)
CONDS = {"other": ("bvult", "bool", Z, ("const", 2, 2)),
         "branches": ("bvult", "bool", X, Y)}
OPS = ["bvult", "bvule", "bvugt", "bvuge", "=", "distinct"]
WRAPS = {"none": lambda t: t,
         "zext": lambda t: ("zero_extend", W + 2, t),
         "sext": lambda t: ("sign_extend", W + 2, t)}


@pytest.mark.parametrize("op", OPS)
@pytest.mark.parametrize("self_branch", ["then", "else"])
@pytest.mark.parametrize("ite_left", [True, False])
@pytest.mark.parametrize("wrap", sorted(WRAPS))
@pytest.mark.parametrize("cond", sorted(CONDS))
def test_equivalent(op, self_branch, ite_left, wrap, cond):
    ite = ("ite", W, CONDS[cond], X, Y)
    e = X if self_branch == "then" else Y
    f = WRAPS[wrap]
    a, b = (f(ite), f(e)) if ite_left else (f(e), f(ite))
    _check([(op, "bool", a, b)], {"x": W, "y": W, "z": 2})


def test_under_structure():
    """Negated, beside another constraint on the same branch, and twice."""
    ite = ("ite", W, CONDS["other"], X, Y)
    _check([("not", "bool", ("bvult", "bool", ite, Y)),
            ("bvuge", "bool", X, ("const", 2, W))], {"x": W, "y": W, "z": 2})
    _check([("or", "bool", ("bvult", "bool", ite, Y), ("=", "bool", ite, X)),
            ("distinct", "bool", ite, Y)], {"x": W, "y": W, "z": 2})


def test_mismatched_extensions_are_left_alone():
    """zext(ite) < sext(y) is not the comparison of what they extend."""
    ite = ("ite", W, CONDS["other"], X, Y)
    for ops in itertools.product(["bvult", "bvuge"], [("zero_extend", "sign_extend"),
                                                      ("sign_extend", "zero_extend")]):
        op, (l, r) = ops
        _check([(op, "bool", (l, W + 2, ite), (r, W + 2, Y))], {"x": W, "y": W, "z": 2})


@pytest.mark.parametrize("op", ["<", "<=", ">", ">=", "==", "!="])
@pytest.mark.parametrize("self_branch", ["then", "else"])
def test_signed(op, self_branch):
    """Signed branches: the comparison keeps its signedness."""
    import operator
    from dv_solve.problem import BIN_EQ, BIN_GT, BIN_GTE, BIN_LT, BIN_LTE, BIN_NEQ
    bop = {"<": BIN_LT, "<=": BIN_LTE, ">": BIN_GT, ">=": BIN_GTE, "==": BIN_EQ, "!=": BIN_NEQ}
    pyop = {"<": operator.lt, "<=": operator.le, ">": operator.gt, ">=": operator.ge,
            "==": operator.eq, "!=": operator.ne}[op]
    b = SolveProblemBuilder()
    b.add_var(0, width=W, is_signed=True, lo=-4, hi=3)
    b.add_var(1, width=W, is_signed=True, lo=-4, hi=3)
    b.add_var(2, width=2, is_signed=False, lo=0, hi=3)
    cond = b.expr_binary(BIN_LT, b.expr_var(2), b.expr_const(2, width=2))
    ite = b.expr_ite(cond, b.expr_var(0), b.expr_var(1))
    e = b.expr_var(0 if self_branch == "then" else 1)
    b.add_constraint(b.expr_binary(bop[op], ite, e))
    with SolveCtx(b.finalize()[0]) as ctx:
        for xv, yv, zv in itertools.product(range(-4, 4), range(-4, 4), range(4)):
            r = xv if zv < 2 else yv
            want = pyop(r, xv if self_branch == "then" else yv)
            ctx.reset()
            cp = ctx.checkpoint()
            ok = ctx.pin(0, xv) and ctx.pin(1, yv) and ctx.pin(2, zv)
            rc = ctx.solve(seed=1) if ok else SOLVE_UNSAT
            ctx.restore(cp)
            assert (rc == SOLVE_OK) == want, (op, xv, yv, zv, rc)


@pytest.mark.parametrize("width", [32, 64])
def test_pinned_solve_is_sat(width):
    """The B103 shape, x pinned high: sat at once, not a climb to a timeout."""
    from tests.formal.soundness.doors import _lower
    x, y, z = (("var", n, width) for n in "xyz")
    c = ("bvult", "bool", ("ite", width, ("bvult", "bool", ("const", 0, width), z), x, y), y)
    b = SolveProblemBuilder()
    for i in range(3):
        b.add_var(i, width=width, is_signed=False, lo=0, hi=(1 << width) - 1)
    b.add_constraint(_lower(b, c, {"x": 0, "y": 1, "z": 2}))
    mask = (1 << width) - 1
    with SolveCtx(b.finalize()[0]) as ctx:
        for v in ((1 << (width - 1)) + 12345, 5, mask - 1):
            cp = ctx.checkpoint()
            assert ctx.pin(0, v)
            assert ctx.solve(seed=1, max_restarts=5, time_limit_ms=1000) == SOLVE_OK, hex(v)
            xv, yv, zv = (ctx.get_value(i) & mask for i in range(3))
            assert xv == v and zv != 0 and xv < yv
            ctx.restore(cp)
        cp = ctx.checkpoint()
        assert not ctx.pin(0, mask) or ctx.solve(seed=1) == SOLVE_UNSAT
        ctx.restore(cp)


@pytest.mark.parametrize("k", [3, 5])
@pytest.mark.parametrize("op", ["bvult", "bvuge", "=", "distinct"])
def test_constant_branch(k, op):
    """ite(c, x, 3) against 3 is the self case; against 5 it is not."""
    ite = ("ite", W, CONDS["other"], X, ("const", 3, W))
    _check([(op, "bool", ite, ("const", k, W))], {"x": W, "y": W, "z": 2})


@pytest.mark.parametrize("ext", [("zext", 0), ("sext", 1)])
@pytest.mark.parametrize("op", ["<", ">="])
def test_signed_under_extension(ext, op):
    """A zero extension of a signed ite does not keep the signed order: -1
    extends to 7. Only the sign extension is looked through."""
    import operator
    from dv_solve.problem import BIN_GTE, BIN_LT
    name, sx = ext
    pyop = {"<": operator.lt, ">=": operator.ge}[op]
    b = SolveProblemBuilder()
    b.add_var(0, width=W, is_signed=True, lo=-4, hi=3)
    b.add_var(1, width=W, is_signed=True, lo=-4, hi=3)
    b.add_var(2, width=2, is_signed=False, lo=0, hi=3)
    cond = b.expr_binary(BIN_LT, b.expr_var(2), b.expr_const(2, width=2))
    ite = b.expr_extend(b.expr_ite(cond, b.expr_var(0), b.expr_var(1)), W, W + 2, bool(sx))
    e = b.expr_extend(b.expr_var(1), W, W + 2, bool(sx))
    b.add_constraint(b.expr_binary({"<": BIN_LT, ">=": BIN_GTE}[op], ite, e))

    def ext_val(v):           # the extended bits, read in the extension's signedness
        u = v & 7
        return v if sx else u

    from dv_solve.ctx import CompileIncompleteError
    try:
        ctx = SolveCtx(b.finalize()[0])
    except CompileIncompleteError:
        assert not sx          # the zero extension may decline; looked through, it compiled wrong
        return
    with ctx:
        for xv, yv, zv in itertools.product(range(-4, 4), range(-4, 4), range(4)):
            r = xv if zv < 2 else yv
            want = pyop(ext_val(r), ext_val(yv))
            ctx.reset()
            cp = ctx.checkpoint()
            ok = ctx.pin(0, xv) and ctx.pin(1, yv) and ctx.pin(2, zv)
            rc = ctx.solve(seed=1) if ok else SOLVE_UNSAT
            ctx.restore(cp)
            assert (rc == SOLVE_OK) == want, (name, op, xv, yv, zv, rc)
