"""CDCL compile gaps closed by docs/cdcl_construct_plan.md phase 1.

Each shape here used to be left uncompiled by the CDCL engine. The SMT-LIB2
front end then routed it to bit-blasting, and the API (which has no
bit-blaster) answered `unknown`. Every check runs pure CDCL -- SMT-LIB2 with
DV_NO_BITBLAST=1, and the Python builder -- and requires a definitive answer
that matches brute force, with a model that satisfies the constraints.

  1.1  `~c` / `-c` over a constant: the divide-by-zero value of bvudiv (and so
       bvsdiv, bvsrem, bvsmod) is `~0`, so every division by a variable.
  1.2  membership with a non-constant element, bound or value.
  1.3  an OR whose leaves are not comparisons (`b1 & b2`, a 1-bit ite).
  1.4  builder `~x` keeps its exact range (bnot), not `x ^ all-ones`.
"""
from __future__ import annotations

import itertools
import os
import subprocess
from pathlib import Path

import pytest

from tests.formal.soundness.doors import _lower
from tests.formal.soundness.ir import Problem, ev, satisfiable

_REPO = Path(__file__).resolve().parents[2]
_DV = Path(os.environ.get("DV_SMT2_EXE", _REPO / "build" / "dv-solve-smt2"))
_ENV = {**{k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"},
        "DV_NO_BITBLAST": "1", "DV_LOG": "1"}


def V(n, w): return ("var", n, w)
def C(v, w): return ("const", v, w)
def T(op, w, *a): return (op, w, *a)


def _smt2_pure_cdcl(p: Problem):
    """Verdict, model and stderr of a pure-CDCL SMT-LIB2 run."""
    r = subprocess.run([str(_DV)], input=p.smt2(), capture_output=True, text=True,
                       timeout=60, env=_ENV)
    words = r.stdout.split()
    got = words[0] if words else "error"
    m = {}
    import re
    for name, val in re.findall(r"\((\w+) #b([01]+)\)", r.stdout):
        m[name] = int(val, 2)
    for name, val in re.findall(r"\((\w+) #x([0-9a-fA-F]+)\)", r.stdout):
        m[name] = int(val, 16)
    return got, m, r.stderr


def _check_smt2(widths, cons):
    if not _DV.is_file():
        pytest.skip("dv-solve-smt2 not built")
    p = Problem(widths, cons)
    want = "sat" if satisfiable(widths, cons) else "unsat"
    got, m, err = _smt2_pure_cdcl(p)
    assert "cdcl-uncompiled" not in err, err
    assert got == want, f"pure CDCL says {got}, brute force {want}\n{p.smt2()}\n{err[-800:]}"
    if got == "sat":
        assert all(ev(c, m) for c in cons), (m, cons)


# ---- 1.1: `~c` / `-c` folding: division by a variable ----

DIV_OPS = ["bvudiv", "bvurem", "bvsdiv", "bvsrem", "bvsmod"]


@pytest.mark.parametrize("op", DIV_OPS)
@pytest.mark.parametrize("k", [0, 1, 7, 15])
def test_division_by_a_variable_compiles(op, k):
    """`x op y == k` with y free (y == 0 included) at 4 bits."""
    x, y = V("x", 4), V("y", 4)
    _check_smt2({"x": 4, "y": 4}, [T("=", "bool", T(op, 4, x, y), C(k, 4))])


@pytest.mark.parametrize("op", DIV_OPS)
def test_division_by_a_variable_ordering(op):
    """A division under an ordering, beside a bound on the divisor."""
    x, y, z = V("x", 4), V("y", 4), V("z", 4)
    _check_smt2({"x": 4, "y": 4, "z": 4},
                [T("bvult", "bool", y, C(3, 4)),
                 T("bvugt", "bool", T(op, 4, x, y), z),
                 T("bvuge", "bool", z, C(9, 4))])


@pytest.mark.parametrize("un", ["bvnot", "bvneg"])
@pytest.mark.parametrize("c", [0, 1, 5, 8, 15])
def test_unary_constant_folds(un, c):
    """`x == ~c`, `x == -c`, and the same under an ite, both doors."""
    x, b = V("x", 4), V("b", 1)
    cons = [T("=", "bool", x, T("ite", 4, T("=", "bool", b, C(1, 1)),
                                 T(un, 4, C(c, 4)), C(3, 4))),
            T("=", "bool", b, C(1, 1))]
    _check_smt2({"x": 4, "b": 1}, cons)
    _check_builder({"x": 4, "b": 1}, cons)


# ---- builder door ----

dv = pytest.importorskip("dv_solve")
from dv_solve.builder import SolveProblemBuilder                      # noqa: E402
from dv_solve.ctx import (CompileIncompleteError, CompileUnsatError,  # noqa: E402
                          SOLVE_OK, SOLVE_UNSAT, SolveCtx)
from dv_solve.problem import (BIN_BAND, BIN_EQ, BIN_LT, BIN_NEQ, BIN_OR,  # noqa: E402
                              UN_INVERT)


def _check_builder(widths, cons):
    ids = {n: i for i, n in enumerate(widths)}
    b = SolveProblemBuilder()
    for n, w in widths.items():
        b.add_var(ids[n], width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
    for c in cons:
        b.add_constraint(_lower(b, c, ids))
    _solve_builder(b, widths, ids, lambda m: all(ev(c, m) for c in cons))


def _solve_builder(b, widths, ids, holds, seeds=4):
    """Compile (no constraint may be left out), then compare with brute force:
    unsat exactly when no assignment holds, every model holds."""
    prob, _ = b.finalize()
    names = list(widths)
    any_sol = any(holds(dict(zip(names, vals))) for vals in
                  itertools.product(*[range(1 << widths[n]) for n in names]))
    try:
        ctx = SolveCtx(prob)
    except CompileUnsatError:
        assert not any_sol, "compile said unsat"
        return
    except CompileIncompleteError as e:
        pytest.fail(f"not compiled: {e}")
    with ctx:
        for s in range(seeds):
            ctx.reset()
            rc = ctx.solve(seed=s, time_limit_ms=10000)
            assert rc in (SOLVE_OK, SOLVE_UNSAT), rc
            assert (rc == SOLVE_OK) == any_sol
            if rc == SOLVE_OK:
                m = {n: ctx.get_value(ids[n]) & ((1 << w) - 1) for n, w in widths.items()}
                assert holds(m), m


# ---- 1.2: membership with non-constant members ----

def _member_problem(kind, neg):
    """x, y, z at 3 bits; `x inside {...}` / `x in [..]` with y (and z) as
    members, plus side constraints that make the answer depend on them."""
    widths = {"x": 3, "y": 3, "z": 3}
    b = SolveProblemBuilder()
    for i, n in enumerate(widths):
        b.add_var(i, width=3, is_signed=False, lo=0, hi=7)
    x, y, z = (b.expr_var(i) for i in range(3))
    c = lambda v: b.expr_const(v, width=3)                      # noqa: E731
    if kind == "set":
        m = b.expr_in_set(x, [y, c(5)])
        f = lambda e: e["x"] in (e["y"], 5)                     # noqa: E731
    elif kind == "range":
        m = b.expr_in_range(x, y, c(6))
        f = lambda e: e["y"] <= e["x"] <= 6                     # noqa: E731
    elif kind == "range-hi":
        m = b.expr_in_range(x, c(1), z)
        f = lambda e: 1 <= e["x"] <= e["z"]                     # noqa: E731
    elif kind == "ranges":
        m = b.expr_in_ranges(x, [(c(0), c(1)), (y, z)])
        f = lambda e: e["x"] <= 1 or e["y"] <= e["x"] <= e["z"]  # noqa: E731
    else:  # value is an expression
        m = b.expr_in_set(b.expr_binary(BIN_BAND, x, c(6)), [c(2), c(4)])
        f = lambda e: (e["x"] & 6) in (2, 4)                    # noqa: E731
    if neg:
        from dv_solve.problem import UN_NOT
        m = b.expr_unary(UN_NOT, m)
        g = f
        f = lambda e: not g(e)                                  # noqa: E731
    b.add_constraint(m)
    b.add_constraint(b.expr_binary(BIN_NEQ, x, c(5)))
    b.add_constraint(b.expr_binary(BIN_LT, y, c(4)))
    return b, widths, (lambda e: f(e) and e["x"] != 5 and e["y"] < 4)


@pytest.mark.parametrize("neg", [False, True])
@pytest.mark.parametrize("kind", ["set", "range", "range-hi", "ranges", "expr"])
def test_nonconstant_membership_compiles(kind, neg):
    b, widths, holds = _member_problem(kind, neg)
    _solve_builder(b, widths, {n: i for i, n in enumerate(widths)}, holds)


def test_nonconstant_membership_smt2_forms():
    """The SMT-LIB2 spelling of the same idea: an or of equalities with a
    variable member, and a range with a variable bound."""
    x, y, z = V("x", 4), V("y", 4), V("z", 4)
    _check_smt2({"x": 4, "y": 4, "z": 4},
                [T("or", "bool", T("=", "bool", x, y), T("=", "bool", x, C(5, 4))),
                 T("distinct", "bool", x, C(5, 4)),
                 T("bvule", "bool", y, x), T("bvule", "bool", x, z),
                 T("bvult", "bool", z, C(9, 4))])


# ---- 1.3: OR leaves that are not comparisons ----

def _or_leaf_problem():
    """svcorpus shape: (b1 & b2) || ite(x == 5, 1, 0) || (b1 ^ b3), with the
    other leaves pushed false part of the time."""
    widths = {"a": 1, "b": 1, "c": 1, "x": 3}
    b = SolveProblemBuilder()
    for i, (n, w) in enumerate(widths.items()):
        b.add_var(i, width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
    a, bb, cc, x = (b.expr_var(i) for i in range(4))
    one = b.expr_const(1, width=1)
    zero = b.expr_const(0, width=1)
    from dv_solve.problem import BIN_BXOR
    leaf1 = b.expr_binary(BIN_BAND, a, bb)
    leaf2 = b.expr_ite(b.expr_binary(BIN_EQ, x, b.expr_const(5, width=3)), one, zero)
    leaf3 = b.expr_binary(BIN_BXOR, a, cc)
    b.add_constraint(b.expr_binary(BIN_OR, leaf1, b.expr_binary(BIN_OR, leaf2, leaf3)))
    b.add_constraint(b.expr_binary(BIN_NEQ, a, cc))   # leaf3 forced true ...
    return b, widths, (lambda e: ((e["a"] & e["b"]) or e["x"] == 5 or (e["a"] ^ e["c"]))
                       and e["a"] != e["c"])


def test_or_with_boolean_leaves_compiles():
    b, widths, holds = _or_leaf_problem()
    _solve_builder(b, widths, {n: i for i, n in enumerate(widths)}, holds, seeds=8)


@pytest.mark.parametrize("force", ["none", "x", "ab"])
def test_or_with_boolean_leaves_smt2(force):
    a, b_, c, x = V("a", 1), V("b", 1), V("c", 1), V("x", 3)
    one = C(1, 1)
    cons = [T("or", "bool",
              T("=", "bool", T("bvand", 1, a, b_), one),
              T("ite", "bool", T("=", "bool", x, C(5, 3)), ("true",), ("false",)),
              T("and", "bool", T("=", "bool", a, one), T("=", "bool", c, one)))]
    if force == "x":
        cons += [T("=", "bool", a, C(0, 1))]
    if force == "ab":
        cons += [T("=", "bool", a, C(0, 1)), T("distinct", "bool", x, C(5, 3))]
    _check_smt2({"a": 1, "b": 1, "c": 1, "x": 3}, cons)


# ---- 1.4: builder `~x` is exact ----

@pytest.mark.parametrize("wx,wr,signed", [(8, 8, False), (8, 16, False), (8, 8, True),
                                          (32, 32, False), (64, 64, False)])
def test_builder_invert_range_is_exact(wx, wr, signed):
    """x in [2, 10]; r == ~x (at r's width); `r < ~10` must be unsat at
    compile time or by propagation alone -- no search."""
    b = SolveProblemBuilder()
    lo_r = -(1 << (wr - 1)) if signed else 0
    hi_r = (1 << (wr - 1)) - 1 if signed else (1 << wr) - 1
    b.add_var(0, width=wx, is_signed=signed, lo=2, hi=10)
    b.add_var(1, width=wr, is_signed=signed, lo=lo_r, hi=hi_r)
    inv = b.expr_unary(UN_INVERT, b.expr_var(0))
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1), inv))
    m = (1 << wr) - 1
    bound = -11 if signed else (~10 & m)        # ~10 at r's width
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(1),
                                   b.expr_const(bound, is_signed=signed, width=wr)))
    prob, _ = b.finalize()
    try:
        ctx = SolveCtx(prob)
    except CompileUnsatError:
        return
    with ctx:
        assert ctx.propagate_only() == 1, "bnot did not bound r: ~x needed search"


def test_builder_invert_models():
    """r == ~x over the whole domain at 4 bits, both signednesses."""
    for signed in (False, True):
        b = SolveProblemBuilder()
        lo, hi = (-8, 7) if signed else (0, 15)
        b.add_var(0, width=4, is_signed=signed, lo=lo, hi=hi)
        b.add_var(1, width=4, is_signed=signed, lo=lo, hi=hi)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1),
                                       b.expr_unary(UN_INVERT, b.expr_var(0))))
        prob, _ = b.finalize()
        with SolveCtx(prob) as ctx:
            seen = set()
            for s in range(40):
                ctx.reset()
                assert ctx.solve(seed=s) == SOLVE_OK
                x, r = ctx.get_value(0), ctx.get_value(1)
                assert (r & 15) == (~x & 15), (signed, x, r)
                seen.add(x)
            assert len(seen) > 4


# The Verilator spelling, as in the svcorpus captures (jtag_item, hmac_base,
# ...): a reified range `lo <= c <= hi` or-ed with `c == 0`, each comparison
# lifted to a bit by __Vbv, the conjunction a bitwise and. The disjunction's
# first leaf is `bvand` of two guards, not a comparison.
_VBV = """(set-logic QF_BV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(declare-fun c () (_ BitVec 32))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge c #x00000001)) (__Vbv (bvule c #x00000028)))
                     (__Vbv (= c #x00000000)))))
{extra}
(check-sat)
(get-value (c))
"""


@pytest.mark.parametrize("extra,want,ok", [
    ("", "sat", lambda c: c <= 40),
    ("(assert (distinct c #x00000000))", "sat", lambda c: 1 <= c <= 40),
    ("(assert (bvugt c #x00000028))", "unsat", None),
    ("(assert (bvugt c #x00000020))", "sat", lambda c: 33 <= c <= 40),
])
def test_verilator_reified_or_of_and(extra, want, ok):
    if not _DV.is_file():
        pytest.skip("dv-solve-smt2 not built")
    r = subprocess.run([str(_DV)], input=_VBV.format(extra=extra), capture_output=True,
                       text=True, timeout=60, env=_ENV)
    assert "cdcl-uncompiled" not in r.stderr, r.stderr
    words = r.stdout.split()
    assert words and words[0] == want, (r.stdout, r.stderr[-500:])
    if ok:
        import re
        m = re.search(r"#([bx])([0-9a-fA-F]+)", r.stdout)
        c = int(m.group(2), 2 if m.group(1) == "b" else 16)
        assert ok(c), c
