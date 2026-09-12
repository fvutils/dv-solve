"""The aggregate constructs the other two probes never reached.

`SUM`, `COUNTONES`, `CLOG2` and `ARRAY_SELECT` are reachable only through the
growable builder API (`dv_solve.builder.SolveProblemBuilder`), not through
`SolveProblem`, which is why neither `expr_coverage_probe.py` nor
`feature_coverage_probe.py` covers them -- both build through `SolveProblem`.
They were G11 in the gap list, the "known unknown": the bit-blaster rejects all
four outright, and the propagator engine's support for them was never measured.

This measures it. Per row: whether the constraint compiled, and whether the
returned assignment actually satisfies it, checked in Python.

The bb column is expected to read UNKNOWN throughout -- these node kinds are not
bit-blasted. That is a deferral, not a wrong answer, and it is recorded here so
the expectation is explicit rather than assumed.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_EQ, BIN_LT, BIN_GTE, BIN_LTE
from dv_solve.ctx import (SolveCtx, CompileIncompleteError, CompileUnsatError,
                          CompileUnsupportedError)
from dv_solve import bvsat

CASES = []


def case(cid, desc, build, check, n_vars):
    CASES.append(dict(cid=cid, desc=desc, build=build, check=check,
                      n_vars=n_vars))


def _mk(c):
    b = SolveProblemBuilder()
    c["build"](b)
    return b


def run_cdcl(c):
    b = _mk(c)
    try:
        buf, _sz = b.finalize()
        try:
            ctx = SolveCtx(buf)
        except CompileUnsupportedError:
            return "UNSUPPORTED"
        except CompileIncompleteError:
            return "INCOMPLETE"
        except CompileUnsatError:
            return "UNSAT-AT-COMPILE"
        except Exception as e:
            return "ERR:" + type(e).__name__
        try:
            if ctx.solve(seed=1) != 0:
                return "NO-SOLUTION"
            vals = [ctx.get_value(i) for i in range(c["n_vars"])]
            if ctx.validate_model() != 0:
                return "INVALID %s" % (vals[:4],)
            return "ok" if c["check"](vals) else "WRONG %s" % (vals[:4],)
        finally:
            ctx.destroy()
    finally:
        b.destroy()


def run_bb(c):
    b = _mk(c)
    try:
        buf, _sz = b.finalize()
        try:
            bb = bvsat.BVSatCtx(buf)
        except Exception as e:
            return "ERR:" + type(e).__name__
        try:
            rc = bb.check(seed=1)
            if rc != bvsat.BVSAT_SAT:
                return {bvsat.BVSAT_UNSAT: "UNSAT",
                        bvsat.BVSAT_UNKNOWN: "UNKNOWN"}.get(rc, "rc=%s" % rc)
            vals = [bb.value(i) for i in range(c["n_vars"])]
            return "ok" if c["check"](vals) else "WRONG %s" % (vals[:4],)
        finally:
            bb.destroy()
    finally:
        b.destroy()


# --- SUM: r == v0 + v1 + ... ---------------------------------------------
def _sum3(b):
    b.add_var(0, 16, False, 0, 300)          # result
    for i in (1, 2, 3):
        b.add_var(i, 8, False, 0, 255)
    b.add_constraint(b.expr_sum(b.expr_var(0),
                                [b.expr_var(1), b.expr_var(2), b.expr_var(3)]))


case("S1", "r == a + b + c   (n-ary SUM, 3 terms)", _sum3,
     lambda v: v[0] == v[1] + v[2] + v[3], 4)


def _sum_pinned(b):
    b.add_var(0, 16, False, 100, 100)        # result pinned to 100
    for i in (1, 2):
        b.add_var(i, 8, False, 0, 255)
    b.add_constraint(b.expr_sum(b.expr_var(0), [b.expr_var(1), b.expr_var(2)]))


case("S2", "100 == a + b   (SUM with the result pinned)", _sum_pinned,
     lambda v: v[1] + v[2] == 100, 3)


# --- COUNTONES ------------------------------------------------------------
def _countones(b):
    b.add_var(0, 8, False, 0, 8)             # result
    b.add_var(1, 8, False, 0, 255)
    b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))


case("C1", "r == countones(x)", _countones,
     lambda v: v[0] == bin(v[1] & 0xFF).count("1"), 2)


def _countones_pinned(b):
    b.add_var(0, 8, False, 3, 3)             # exactly 3 bits set
    b.add_var(1, 8, False, 0, 255)
    b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))


case("C2", "countones(x) == 3   (result pinned)", _countones_pinned,
     lambda v: bin(v[1] & 0xFF).count("1") == 3, 2)


# --- CLOG2 ----------------------------------------------------------------
def _clog2(b):
    b.add_var(0, 8, False, 0, 8)             # result
    b.add_var(1, 16, False, 1, 1000)
    b.add_constraint(b.expr_clog2(b.expr_var(0), b.expr_var(1)))


def _py_clog2(x):
    if x <= 1:
        return 0
    return (x - 1).bit_length()


case("L1", "r == clog2(x)", _clog2,
     lambda v: v[0] == _py_clog2(v[1]), 2)


# --- ARRAY_SELECT ---------------------------------------------------------
def _array_select(b):
    # vars 0..3 are the array elements, 4 is the index, 5 is the result.
    for i in range(4):
        b.add_var(i, 8, False, 10 * i, 10 * i)   # elements pinned: 0,10,20,30
    b.add_var(4, 8, False, 0, 3)                 # index
    b.add_var(5, 8, False, 0, 255)               # result
    b.add_constraint(b.expr_array_select(0, 4, b.expr_var(5), b.expr_var(4)))


case("A1", "r == arr[i]   (elements pinned 0,10,20,30)", _array_select,
     lambda v: v[5] == 10 * v[4], 6)


def _array_select_pinned(b):
    for i in range(4):
        b.add_var(i, 8, False, 10 * i, 10 * i)
    b.add_var(4, 8, False, 0, 3)
    b.add_var(5, 8, False, 20, 20)               # result pinned -> index must be 2
    b.add_constraint(b.expr_array_select(0, 4, b.expr_var(5), b.expr_var(4)))


case("A2", "arr[i] == 20   (result pinned; index must solve to 2)",
     _array_select_pinned, lambda v: v[4] == 2, 6)


def main():
    w = max(len(c["desc"]) for c in CASES)
    print("%-4s %-*s  %-18s %s" % ("id", w, "case", "cdcl", "bb"))
    for c in CASES:
        try:
            a = run_cdcl(c)
        except Exception as e:
            a = "PROBE-BUG:" + type(e).__name__
        try:
            bbr = run_bb(c)
        except Exception as e:
            bbr = "PROBE-BUG:" + type(e).__name__
        print("%-4s %-*s  %-18s %s" % (c["cid"], w, c["desc"], a, bbr))


if __name__ == "__main__":
    main()
