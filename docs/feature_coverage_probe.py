"""Feature-axis sweep: the parts of dv-solve the expression probe never touched.

`expr_coverage_probe.py` varies *expression shape* at one width, unsigned, with
plain hard constraints. This varies the other axes — signedness, width tier,
set/range membership, bitwise/shift/divide, AllDifferent, soft constraints,
`dist`, and the two solve entry points — so the gap list is a statement about
dv-solve rather than about one PSS construct.

Per row, for each engine: whether it compiled, and whether the returned
assignment actually satisfies the constraint. `WRONG` is the status that matters
most — nothing on this path re-checks the model, so a wrong answer is silent.

Two API details this probe has to get right, because getting them wrong produces
convincing-looking false failures (both were made and caught while writing it):

* `expr_in_range(value, lo, hi)` and `expr_in_set(value, elems)` take **ExprRefs**
  for their bounds and elements, not Python ints. Passing ints indexes the
  expression pool at an arbitrary offset.
* Values read back through `solve_n`/`get_value`/`value` are **int64**, so an
  unsigned value above 2^63 arrives negative. The checks below mask to the
  declared width; see the R-A row for the gap that makes this necessary.
"""
import ctypes
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from dv_solve.problem import (
    SolveProblem, BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV, BIN_BAND, BIN_BOR,
    BIN_BXOR, BIN_LSHIFT, BIN_RSHIFT, BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE,
    BIN_GT, BIN_GTE, BIN_OR)
from dv_solve.ctx import (SolveCtx, CompileIncompleteError, CompileUnsatError,
                          CompileUnsupportedError)
from dv_solve import bvsat

UN_NOT = 1
CASES = []


def case(cid, desc, nvars, width, signed, build, check, *,
         n_solves=4, batch=True):
    """Register a probe row.

    *batch* selects the solve entry point: True uses `solve_n` (what a stimulus
    generator uses), False uses a single `solve`. It is a parameter because the
    two disagree — see the D rows.
    """
    CASES.append(dict(cid=cid, desc=desc, nvars=nvars, width=width,
                      signed=signed, build=build, check=check,
                      n_solves=n_solves, batch=batch))


def _mk(c):
    sp = SolveProblem()
    w, signed = c["width"], c["signed"]
    lo = -(1 << (w - 1)) if signed else 0
    hi = (1 << (w - 1)) - 1 if signed else (1 << w) - 1
    # add_var's lo/hi are int64, so a domain wider than int64 cannot be
    # expressed at all -- clamp and let the width-tier rows report what happens.
    lo = max(lo, -(1 << 63))
    hi = min(hi, (1 << 63) - 1)
    for i in range(c["nvars"]):
        sp.add_var(i, width=w, is_signed=signed, lo=lo, hi=hi)
    c["build"](sp)
    return sp


def _norm(c, vals):
    """Read int64-returned values back as the declared type."""
    if c["signed"]:
        return list(vals)
    mask = (1 << min(c["width"], 64)) - 1
    return [v & mask for v in vals]


def run_cdcl(c):
    try:
        ctx = SolveCtx(_mk(c))
    except CompileUnsupportedError:
        # Ordered before CompileIncompleteError, which it subclasses.
        return "UNSUPPORTED"
    except CompileIncompleteError:
        return "INCOMPLETE"
    except CompileUnsatError:
        return "UNSAT-AT-COMPILE"
    except Exception as e:
        return "ERR:" + type(e).__name__
    try:
        if c["batch"]:
            ids = (ctypes.c_uint32 * c["nvars"])(*range(c["nvars"]))
            n_ok, sols = ctx.solve_n(c["n_solves"], ids, c["nvars"], base_seed=1)
            if n_ok == 0:
                return "NO-SOLUTION"
        else:
            if ctx.solve(seed=1) != 0:
                return "NO-SOLUTION"
            sols = [[ctx.get_value(i) for i in range(c["nvars"])]]
        for vals in sols:
            v = _norm(c, vals)
            if not c["check"](v):
                return "WRONG %s" % (v[:3],)
        return "ok"
    finally:
        ctx.destroy()


def run_bb(c):
    try:
        bb = bvsat.BVSatCtx(_mk(c))
    except Exception as e:
        return "ERR:" + type(e).__name__
    try:
        rc = bb.check(seed=1)
        if rc != bvsat.BVSAT_SAT:
            return {bvsat.BVSAT_UNSAT: "UNSAT",
                    bvsat.BVSAT_UNKNOWN: "UNKNOWN"}.get(rc, "rc=%s" % rc)
        vals = [bb.value_wide(i, c["width"], c["signed"])
                for i in range(c["nvars"])]
        return "ok" if c["check"](_norm(c, vals)) else "WRONG %s" % (vals[:3],)
    finally:
        bb.destroy()


def cst(sp, v):
    return sp.expr_const(v)


# --- S: signedness (PSS `int` is 32-bit signed; negatives are the risk) -----
case("S1", "signed: x < 0", 2, 32, True,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_LT, sp.expr_var(0), cst(sp, 0))),
     lambda v: v[0] < 0)
case("S2", "signed: x == a, a < -100", 2, 32, True,
     lambda sp: (sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0), sp.expr_var(1))),
                 sp.add_constraint(sp.expr_binary(BIN_LT, sp.expr_var(1), cst(sp, -100)))),
     lambda v: v[0] == v[1] and v[1] < -100)
case("S3", "signed: x == a + 1, a negative", 2, 32, True,
     lambda sp: (sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                     sp.expr_binary(BIN_ADD, sp.expr_var(1), cst(sp, 1)))),
                 sp.add_constraint(sp.expr_binary(BIN_LT, sp.expr_var(1), cst(sp, -10)))),
     lambda v: v[0] == v[1] + 1 and v[1] < -10)
case("S4", "signed: -50 <= x <= -10", 1, 32, True,
     lambda sp: (sp.add_constraint(sp.expr_binary(BIN_GTE, sp.expr_var(0), cst(sp, -50))),
                 sp.add_constraint(sp.expr_binary(BIN_LTE, sp.expr_var(0), cst(sp, -10)))),
     lambda v: -50 <= v[0] <= -10)

# --- W: width tiers (tier0 <32, tier1 32..64, tier2 >64) -------------------
case("W1", "w=32 unsigned: x < a", 2, 32, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_LT, sp.expr_var(0), sp.expr_var(1))),
     lambda v: v[0] < v[1])
case("W2", "w=64 unsigned: x < a", 2, 64, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_LT, sp.expr_var(0), sp.expr_var(1))),
     lambda v: v[0] < v[1])
case("W3", "w=63 unsigned: x == a + 1", 2, 63, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_ADD, sp.expr_var(1), cst(sp, 1)))),
     lambda v: v[0] == (v[1] + 1) % (1 << 63))
case("W4", "w=64 unsigned: x == a + 1", 2, 64, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_ADD, sp.expr_var(1), cst(sp, 1)))),
     lambda v: v[0] == (v[1] + 1) % (1 << 64))
case("W5", "w=64 unsigned: x == a - 1", 2, 64, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_SUB, sp.expr_var(1), cst(sp, 1)))),
     lambda v: v[0] == (v[1] - 1) % (1 << 64))
case("W6", "w=64 signed: x == a + 1  [control]", 2, 64, True,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_ADD, sp.expr_var(1), cst(sp, 1)))),
     lambda v: v[0] == v[1] + 1)
case("W7", "w=64 unsigned: x == a * 2  [control]", 2, 64, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_MUL, sp.expr_var(1), cst(sp, 2)))),
     lambda v: v[0] == (v[1] * 2) % (1 << 64))
case("W8", "w=65: one var, NO constraints at all", 1, 65, False,
     lambda sp: None, lambda v: True)
case("W9", "w=128: one var, NO constraints at all", 1, 128, False,
     lambda sp: None, lambda v: True)

# --- N: the `!=` the builder warns about ("BIN_NEQ has a native solver bug")
case("N1", "x != 5", 1, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_NEQ, sp.expr_var(0), cst(sp, 5))),
     lambda v: v[0] != 5)
case("N2", "x != a", 2, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_NEQ, sp.expr_var(0), sp.expr_var(1))),
     lambda v: v[0] != v[1])
case("N3", "x != 0", 1, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_NEQ, sp.expr_var(0), cst(sp, 0))),
     lambda v: v[0] != 0)

# --- R: set / range membership (PSS `x in [a..b]`) -------------------------
case("R1", "x in_range [20..30]", 1, 8, False,
     lambda sp: sp.add_constraint(sp.expr_in_range(sp.expr_var(0), cst(sp, 20), cst(sp, 30))),
     lambda v: 20 <= v[0] <= 30)
case("R2", "x in_set {3,7,11}", 1, 8, False,
     lambda sp: sp.add_constraint(sp.expr_in_set(
         sp.expr_var(0), [cst(sp, 3), cst(sp, 7), cst(sp, 11)])),
     lambda v: v[0] in (3, 7, 11))
case("R3", "in_range under an OR", 2, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_OR,
                    sp.expr_in_range(sp.expr_var(0), cst(sp, 20), cst(sp, 30)),
                    sp.expr_binary(BIN_GT, sp.expr_var(1), cst(sp, 250)))),
     lambda v: 20 <= v[0] <= 30 or v[1] > 250)
case("R4", "NOT in_range", 1, 8, False,
     lambda sp: sp.add_constraint(sp.expr_unary(UN_NOT,
                    sp.expr_in_range(sp.expr_var(0), cst(sp, 20), cst(sp, 30)))),
     lambda v: not (20 <= v[0] <= 30))

# --- G: aggregates (SUM/COUNTONES/CLOG2/ARRAY_SELECT) reach the solver only
#        through the growable builder API, not SolveProblem, so they are not
#        probed here. bb rejects all four outright (`zsp_bbsolver.c:904`); the
#        cdcl side is unmeasured.

# --- B: bitwise / shift / divide ------------------------------------------
case("B1", "x == a & 0x0f", 2, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_BAND, sp.expr_var(1), cst(sp, 0x0f)))),
     lambda v: v[0] == (v[1] & 0x0f))
case("B2", "x == a | 0x80", 2, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_BOR, sp.expr_var(1), cst(sp, 0x80)))),
     lambda v: v[0] == (v[1] | 0x80))
case("B3", "x == a ^ b", 3, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_BXOR, sp.expr_var(1), sp.expr_var(2)))),
     lambda v: v[0] == (v[1] ^ v[2]))
case("B4", "x == a << 2", 2, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_LSHIFT, sp.expr_var(1), cst(sp, 2)))),
     lambda v: v[0] == ((v[1] << 2) & 0xff))
case("B5", "x == a >> 2", 2, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_RSHIFT, sp.expr_var(1), cst(sp, 2)))),
     lambda v: v[0] == (v[1] >> 2))
case("B6", "x == a / 3", 2, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_DIV, sp.expr_var(1), cst(sp, 3)))),
     lambda v: v[0] == v[1] // 3)
case("B7", "x == a / b, b unconstrained (div-by-zero reachable)", 3, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                    sp.expr_binary(BIN_DIV, sp.expr_var(1), sp.expr_var(2)))),
     lambda v: v[2] == 0 or v[0] == v[1] // v[2])

# --- D: AllDifferent, soft, dist, and the two solve entry points ----------
case("D1", "all_different over 4 vars in [0..3]", 4, 8, False,
     lambda sp: (sp.add_all_different([0, 1, 2, 3]),
                 [sp.add_constraint(sp.expr_binary(BIN_LTE, sp.expr_var(i), cst(sp, 3)))
                  for i in range(4)]),
     lambda v: len(set(v)) == 4 and all(x <= 3 for x in v))
case("D2", "soft conflicting with hard, via solve_n", 1, 8, False,
     lambda sp: (sp.add_constraint(sp.expr_binary(BIN_GT, sp.expr_var(0), cst(sp, 200))),
                 sp.add_soft_constraint(sp.expr_binary(BIN_LT, sp.expr_var(0), cst(sp, 10)))),
     lambda v: v[0] > 200)
case("D3", "soft conflicting with hard, via solve  [same problem]", 1, 8, False,
     lambda sp: (sp.add_constraint(sp.expr_binary(BIN_GT, sp.expr_var(0), cst(sp, 200))),
                 sp.add_soft_constraint(sp.expr_binary(BIN_LT, sp.expr_var(0), cst(sp, 10)))),
     lambda v: v[0] > 200, batch=False)
case("D4", "soft compatible with hard, via solve_n", 1, 8, False,
     lambda sp: (sp.add_constraint(sp.expr_binary(BIN_GT, sp.expr_var(0), cst(sp, 100))),
                 sp.add_soft_constraint(sp.expr_binary(BIN_LT, sp.expr_var(0), cst(sp, 150)))),
     lambda v: v[0] > 100)
case("D5", "hard only, via solve_n  [control for D2/D4]", 1, 8, False,
     lambda sp: sp.add_constraint(sp.expr_binary(BIN_GT, sp.expr_var(0), cst(sp, 200))),
     lambda v: v[0] > 200)
case("D6", "dist over {1, 2, 3}", 1, 8, False,
     lambda sp: sp.add_dist(0, [dict(lo=1, hi=1, weight=1, is_range=0),
                                dict(lo=2, hi=2, weight=1, is_range=0),
                                dict(lo=3, hi=3, weight=8, is_range=0)]),
     lambda v: v[0] in (1, 2, 3), n_solves=8)

def main():
    w = max(len(c["desc"]) for c in CASES)
    print("%-4s %-*s  %-18s %s" % ("id", w, "case", "cdcl", "bb"))
    for c in CASES:
        try:
            a = run_cdcl(c)
        except Exception as e:
            a = "PROBE-BUG:" + type(e).__name__
        try:
            b = run_bb(c)
        except Exception as e:
            b = "PROBE-BUG:" + type(e).__name__
        print("%-4s %-*s  %-18s %s" % (c["cid"], w, c["desc"], a, b))


# Guarded so tests/unit/test_expr_coverage.py can import CASES and assert on
# every row without the table printing as a side effect of the import.
if __name__ == "__main__":
    main()
