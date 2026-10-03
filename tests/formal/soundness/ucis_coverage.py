"""Write the campaign's stimulus coverage as a UCIS database (covsight).

Every bin of the model is declared, hit or not, so `covsight show gaps` lists
what the campaign has never generated. Bin counts are numbers of problems.
Databases from separate runs merge with `covsight merge`; see
docs/soundness_coverage_plan.md §4.4.
"""
from __future__ import annotations

import datetime

from .gen import WIDTH_CLASSES
from .ir import BV_BIN, BV_UN, CMP

OPERATORS = BV_BIN + BV_UN + ["ite", "extract", "concat", "zero_extend", "sign_extend"]

# coverpoint name -> (bin prefix in the generator's bin set, bin names)
COVERPOINTS = {
    "operator": ("op:", OPERATORS),
    "comparison": ("cmp:", CMP),
    "boolean": ("bool:", ["and", "or2", "or3", "or5", "not", "=>", "xor", "ite", "ite-eq-cond"]),
    "operand_shape": ("shape:", ["shared-subterm", "same-operand", "alias", "alias-guarded"]),
    "constant": ("const:", ["zero", "one", "max", "sign-min", "sign-max", "interior"]),
    "problem_size": ("size:", [str(n) for n in range(1, 8)]),
    "width": ("width:", ["narrow", "16", "32", "33", "63", "64", "65"]),
}
DOORS = ["smt2", "smt2-incr", "builder", "protocol"]
ANSWERS = ["sat", "unsat", "unknown", "timeout", "error"]


def write(path: str, bins, stats, test_name: str, seed: int) -> None:
    """bins: Counter of generator bins; stats: Counter of '<door>:<answer>'."""
    from covsight.core.api import (FlagsT, HistoryNodeKind, ScopeTypeT, SourceInfo,
                                   SourceT, TestStatusT)
    from covsight.core.api.test_data import TestData
    from covsight.core.mem import MemFactory
    from covsight.core.ncdb.ncdb_writer import NcdbWriter

    db = MemFactory.create()
    node = db.createHistoryNode(None, test_name, path, HistoryNodeKind.TEST)
    node.setTestData(TestData(
        teststatus=TestStatusT.OK, toolcategory="dv-solve:soundness-campaign",
        date=datetime.datetime.now().strftime("%Y%m%d%H%M%S"), seed=str(seed)))
    fh = db.createFileHandle("tests/formal/soundness/gen.py", None)
    si = SourceInfo(fh, 0, 0)
    du = db.createScope("soundness", si, 1, SourceT.NONE, ScopeTypeT.DU_MODULE,
                        FlagsT.INST_ONCE | FlagsT.SCOPE_UNDER_DU)
    inst = db.createInstance("soundness", None, 1, SourceT.NONE, ScopeTypeT.INSTANCE, du,
                             FlagsT.INST_ONCE)
    cg = inst.createCovergroup("stimulus", si, 1, SourceT.NONE)
    narrow = sum(c for b, c in bins.items() if b.startswith("size:"))
    narrow -= sum(c for b, c in bins.items() if b.startswith("width:"))
    for cp_name, (prefix, names) in COVERPOINTS.items():
        cp = cg.createCoverpoint(cp_name, si, 1, SourceT.NONE)
        for n in names:
            count = max(narrow, 0) if (prefix, n) == ("width:", "narrow") else bins.get(prefix + n, 0)
            cp.createBin(n, si, 1, count, n)
    # Cross operator x width class, flattened to one coverpoint so every
    # combination is a declared bin.
    cp = cg.createCoverpoint("operator_x_width", si, 1, SourceT.NONE)
    for op in OPERATORS:
        for wc in WIDTH_CLASSES:
            name = f"{op}@{wc}"
            cp.createBin(name, si, 1, bins.get(f"opw:{op}:{wc}", 0), name)

    og = inst.createCovergroup("outcome", si, 1, SourceT.NONE)
    cp = og.createCoverpoint("door_x_answer", si, 1, SourceT.NONE)
    for d in DOORS:
        for a in ANSWERS:
            if a in ("timeout", "error"):
                continue    # failures, not coverage goals
            cp.createBin(f"{d}:{a}", si, 1, stats.get(f"{d}:{a}", 0), f"{d}:{a}")
    NcdbWriter().write(db, path)
