"""Which expression shapes each of dv-solve's two engines accepts.

Both are driven from the *same* SolveProblem, built through dv-solve's own
Python builder, so nothing about pssc, ast2ir or be-bc is in the picture.

  cdcl  -- SolveCtx: the bounds/propagator engine, the one the SolveProblem and
           DPI paths use, and therefore the one pssc reaches through be-bc.
  bb    -- BVSatCtx: the bit-blasting completeness engine (zsp_bbsolver),
           reachable from Python today, not reachable from the cdcl path.

Arithmetic is checked modulo 2**8, which is what an 8-bit bit-vector means.
"""
import ctypes
import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from dv_solve.problem import (
    SolveProblem, BIN_ADD, BIN_SUB, BIN_MUL, BIN_MOD, BIN_EQ, BIN_NEQ,
    BIN_LT, BIN_LTE, BIN_GT, BIN_GTE, BIN_AND, BIN_OR)
from dv_solve.ctx import (SolveCtx, CompileIncompleteError, CompileUnsatError,
                          CompileUnsupportedError)
from dv_solve import bvsat

M = 256
NAMES = ["x", "a", "b", "j", "k", "l"]
CASES = []


def case(cid, desc, fn, check):
    CASES.append((cid, desc, fn, check))


def build(fn):
    sp = SolveProblem()
    for i in range(len(NAMES)):
        sp.add_var(i, width=8, is_signed=False, lo=0, hi=255)
    sp.add_constraint(fn(sp, sp.expr_var, sp.expr_const, sp.expr_binary))
    return sp


def try_cdcl(fn, check):
    try:
        sp = build(fn)
        ctx = SolveCtx(sp)
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
        ids = (ctypes.c_uint32 * len(NAMES))(*range(len(NAMES)))
        n_ok, sols = ctx.solve_n(4, ids, len(NAMES), base_seed=1)
        if n_ok == 0:
            return "NO-SOLUTION"
        for vals in sols:
            if not check(dict(zip(NAMES, vals))):
                return "WRONG"
        return "ok"
    finally:
        ctx.destroy()


def try_bb(fn, check):
    try:
        sp = build(fn)
        bb = bvsat.BVSatCtx(sp)
    except Exception as e:
        return "ERR:" + type(e).__name__
    try:
        rc = bb.check(seed=1)
        if rc != bvsat.BVSAT_SAT:
            return {bvsat.BVSAT_UNSAT: "UNSAT", bvsat.BVSAT_UNKNOWN: "UNKNOWN"}.get(rc, f"rc={rc}")
        env = {n: bb.value(i) for i, n in enumerate(NAMES)}
        return "ok" if check(env) else "WRONG"
    finally:
        bb.destroy()


# --- shapes the cdcl engine is known to handle ----------------------------
case("A1", "j == k + 1",
     lambda sp, V, C, B: B(BIN_EQ, V(3), B(BIN_ADD, V(4), C(1))),
     lambda e: e["j"] == (e["k"] + 1) % M)
case("A2", "j == k * 2",
     lambda sp, V, C, B: B(BIN_EQ, V(3), B(BIN_MUL, V(4), C(2))),
     lambda e: e["j"] == (e["k"] * 2) % M)
case("A3", "j == k + l",
     lambda sp, V, C, B: B(BIN_EQ, V(3), B(BIN_ADD, V(4), V(5))),
     lambda e: e["j"] == (e["k"] + e["l"]) % M)
case("A4", "k + 1 == 200  (expr compared to a constant)",
     lambda sp, V, C, B: B(BIN_EQ, B(BIN_ADD, V(4), C(1)), C(200)),
     lambda e: (e["k"] + 1) % M == 200)
case("A5", "x % 4 == 0",
     lambda sp, V, C, B: B(BIN_EQ, B(BIN_MOD, V(0), C(4)), C(0)),
     lambda e: e["x"] % 4 == 0)

# --- arithmetic under a comparison other than a top-level == --------------
case("B1", "x < a + 1",
     lambda sp, V, C, B: B(BIN_LT, V(0), B(BIN_ADD, V(1), C(1))),
     lambda e: e["x"] < (e["a"] + 1) % M)
case("B2", "x <= a + b",
     lambda sp, V, C, B: B(BIN_LTE, V(0), B(BIN_ADD, V(1), V(2))),
     lambda e: e["x"] <= (e["a"] + e["b"]) % M)
case("B3", "x > a - 1",
     lambda sp, V, C, B: B(BIN_GT, V(0), B(BIN_SUB, V(1), C(1))),
     lambda e: e["x"] > (e["a"] - 1) % M)
case("B4", "x != a + 1",
     lambda sp, V, C, B: B(BIN_NEQ, V(0), B(BIN_ADD, V(1), C(1))),
     lambda e: e["x"] != (e["a"] + 1) % M)
case("B5", "a + 1 < x",
     lambda sp, V, C, B: B(BIN_LT, B(BIN_ADD, V(1), C(1)), V(0)),
     lambda e: (e["a"] + 1) % M < e["x"])

# --- nested arithmetic ----------------------------------------------------
case("C1", "j == (k + 1) + 1",
     lambda sp, V, C, B: B(BIN_EQ, V(3), B(BIN_ADD, B(BIN_ADD, V(4), C(1)), C(1))),
     lambda e: e["j"] == (e["k"] + 2) % M)
case("C2", "j == k * 2 + 1",
     lambda sp, V, C, B: B(BIN_EQ, V(3), B(BIN_ADD, B(BIN_MUL, V(4), C(2)), C(1))),
     lambda e: e["j"] == (e["k"] * 2 + 1) % M)
case("C3", "j == (k + l) * 2",
     lambda sp, V, C, B: B(BIN_EQ, V(3), B(BIN_MUL, B(BIN_ADD, V(4), V(5)), C(2))),
     lambda e: e["j"] == ((e["k"] + e["l"]) * 2) % M)
case("D1", "j + 1 == k + 2  (arithmetic on both sides)",
     lambda sp, V, C, B: B(BIN_EQ, B(BIN_ADD, V(3), C(1)), B(BIN_ADD, V(4), C(2))),
     lambda e: (e["j"] + 1) % M == (e["k"] + 2) % M)

# --- constant-only arithmetic (is it folded?) -----------------------------
case("E1", "x == 10 + 10",
     lambda sp, V, C, B: B(BIN_EQ, V(0), B(BIN_ADD, C(10), C(10))),
     lambda e: e["x"] == 20)
case("E2", "x < 19 + 1",
     lambda sp, V, C, B: B(BIN_LT, V(0), B(BIN_ADD, C(19), C(1))),
     lambda e: e["x"] < 20)

# --- arithmetic inside a boolean structure -------------------------------
case("F1", "(j == k + 1) || (x > 200)",
     lambda sp, V, C, B: B(BIN_OR, B(BIN_EQ, V(3), B(BIN_ADD, V(4), C(1))),
                           B(BIN_GT, V(0), C(200))),
     lambda e: e["j"] == (e["k"] + 1) % M or e["x"] > 200)
case("F2", "(x < 10) && (j == k + 1)",
     lambda sp, V, C, B: B(BIN_AND, B(BIN_LT, V(0), C(10)),
                           B(BIN_EQ, V(3), B(BIN_ADD, V(4), C(1)))),
     lambda e: e["x"] < 10 and e["j"] == (e["k"] + 1) % M)
case("F3", "(a != 1) || (j == k + 1)   [an implication]",
     lambda sp, V, C, B: B(BIN_OR, B(BIN_NEQ, V(1), C(1)),
                           B(BIN_EQ, V(3), B(BIN_ADD, V(4), C(1)))),
     lambda e: e["a"] != 1 or e["j"] == (e["k"] + 1) % M)
case("F4", "(x < 10) || (x > 200)   [control: no arithmetic]",
     lambda sp, V, C, B: B(BIN_OR, B(BIN_LT, V(0), C(10)), B(BIN_GT, V(0), C(200))),
     lambda e: e["x"] < 10 or e["x"] > 200)
case("F5", "(k < l) || (x > 200)   [var-var inequality as an OR leaf]",
     lambda sp, V, C, B: B(BIN_OR, B(BIN_LT, V(4), V(5)), B(BIN_GT, V(0), C(200))),
     lambda e: e["k"] < e["l"] or e["x"] > 200)

# --- the ternary ---------------------------------------------------------
case("G1", "j == ((k < l) ? l : k)   [the LRM's own max()]",
     lambda sp, V, C, B: B(BIN_EQ, V(3), sp.expr_ite(B(BIN_LT, V(4), V(5)), V(5), V(4))),
     lambda e: e["j"] == max(e["k"], e["l"]))
case("G2", "j == ((k == 1) ? l : k)   [EQ condition]",
     lambda sp, V, C, B: B(BIN_EQ, V(3), sp.expr_ite(B(BIN_EQ, V(4), C(1)), V(5), V(4))),
     lambda e: e["j"] == (e["l"] if e["k"] == 1 else e["k"]))
case("G3", "j == ((k < 10) ? l : k)   [var-const condition]",
     lambda sp, V, C, B: B(BIN_EQ, V(3), sp.expr_ite(B(BIN_LT, V(4), C(10)), V(5), V(4))),
     lambda e: e["j"] == (e["l"] if e["k"] < 10 else e["k"]))
case("G4", "j == ((k < l) ? l + 1 : k)   [arithmetic in a branch]",
     lambda sp, V, C, B: B(BIN_EQ, V(3), sp.expr_ite(B(BIN_LT, V(4), V(5)),
                                                     B(BIN_ADD, V(5), C(1)), V(4))),
     lambda e: e["j"] == ((e["l"] + 1) % M if e["k"] < e["l"] else e["k"]))

# --- controls ------------------------------------------------------------
case("H1", "x < a   [no arithmetic at all]",
     lambda sp, V, C, B: B(BIN_LT, V(0), V(1)), lambda e: e["x"] < e["a"])
case("H2", "x == a",
     lambda sp, V, C, B: B(BIN_EQ, V(0), V(1)), lambda e: e["x"] == e["a"])

def main():
    w = max(len(c[1]) for c in CASES)
    print("%-4s %-*s  %-17s %s" % ("id", w, "shape", "cdcl (SolveCtx)", "bb (BVSatCtx)"))
    for cid, desc, fn, check in CASES:
        a = try_cdcl(fn, check)
        b = try_bb(fn, check)
        print("%-4s %-*s  %-17s %s" % (cid, w, desc, a, b))


# Guarded so tests/unit/test_expr_coverage.py can import CASES and assert on
# every row without the table printing as a side effect of the import.
if __name__ == "__main__":
    main()
