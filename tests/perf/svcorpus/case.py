"""A corpus case: one SystemVerilog class's constraints as builder API calls.

A case is a module in cases/ with:

  NAME     the case name (also its captures/<NAME>/ directory)
  SOURCE   where the constraints come from: repo, commit, file, lines
  ABOUT    one line on what the constraints do
  VARS     [(name, width, signed)] in declaration order; var ids follow it
  DIST     {name: [(lo, hi, weight, per_value)]}: SV `dist` (`:=` is
           per_value) -- the case adds membership as a hard constraint
           itself, as SV does; these weights go to add_dist
  SOLVE_BEFORE  [(before..., after...)] as written (the builder has no
           ordering; kept for the record and the spread reference)
  build(b, v)   adds the constraints: b is a SolveProblemBuilder, v maps a
           name to its var id
  KNOWN_ISSUE  optional: the backlog entry that keeps the solver from
           solving the case (test_svcorpus expects the failure, strictly)
  DIST_WHEN  optional {name: function(model) -> bool}: a `dist` written in
           one branch of an if/else applies only when the branch is taken;
           the bench's reference draws the variable uniformly otherwise
           (the function may read only variables that have no DIST_WHEN)
  BOX      optional {name: (lo, hi)}: where the bench's rejection-sampled
           reference draws a variable whose solutions are sparse in its
           declared domain (a 32-bit length in [16:32]); equiv.py proves no
           solution lies outside it

The calls in build() mirror the SV source term for term, in SystemVerilog
sizing (an unsized literal is a 32-bit signed int, `parameter byte` an 8-bit
signed one): the builder follows IEEE 1800 sizing, so the native problem is
the class as a front end would hand it over, not Verilator's lowering of it.
captures/<NAME>/ holds the class as a standalone harness (harness.sv) and
the first query Verilator 5.052 sent for it (base.smt2): equiv.py checks the
two have the same solutions.
"""
from __future__ import annotations

import importlib
import pathlib

from dv_solve.problem import BIN_AND, BIN_OR, UN_NOT

HERE = pathlib.Path(__file__).resolve().parent
CASES = HERE / "cases"
CAPTURES = HERE / "captures"


def names() -> list:
    return sorted(p.stem for p in CASES.glob("*.py") if not p.stem.startswith("_"))


def load(name: str):
    return importlib.import_module(f"tests.perf.svcorpus.cases.{name}")


def ids(case) -> dict:
    return {n: i for i, (n, _, _) in enumerate(case.VARS)}


def problem(case, soft_as_hard: bool = False):
    """The case as a finalized builder problem. soft_as_hard: every soft
    constraint added as a hard one -- the form of Verilator's first query,
    which asserts the softs and relaxes them only if that is unsat."""
    from dv_solve.builder import SolveProblemBuilder
    b = SolveProblemBuilder()
    if soft_as_hard:
        b.add_soft_constraint = lambda root, priority=0: b.add_constraint(root)
    v = ids(case)
    for n, w, s in case.VARS:
        lo, hi = (-(1 << (w - 1)), (1 << (w - 1)) - 1) if s else (0, (1 << w) - 1)
        b.add_var(v[n], width=w, is_signed=s, lo=lo, hi=hi)
    case.build(b, v)
    for n, entries in getattr(case, "DIST", {}).items():
        b.add_dist(v[n], [{"lo": lo, "hi": hi, "weight": wt, "is_per_value": pv}
                          for lo, hi, wt, pv in entries])
    return b.finalize()[0]


def capture(case):
    """(widths, constraints) of the Verilator capture, `dist` pins dropped."""
    from tests.perf.svcorpus.vlt import base_query
    text = (CAPTURES / case.NAME / "base.smt2").read_text()
    return base_query(text, dist_vars=list(getattr(case, "DIST", {})))


# ---- helpers for build(): plain SV operators over builder calls ------------

def implies(b, a, c):
    """a -> c"""
    return b.expr_binary(BIN_OR, b.expr_unary(UN_NOT, a), c)


def all_of(b, *xs):
    acc = xs[0]
    for x in xs[1:]:
        acc = b.expr_binary(BIN_AND, acc, x)
    return acc


def inside_dist(b, x, entries, width=32):
    """x inside the ranges of a `dist` (its hard part), int-sized bounds."""
    return b.expr_in_ranges(x, [(b.expr_const(lo, width=width), b.expr_const(hi, width=width))
                                for lo, hi, _, _ in entries])
