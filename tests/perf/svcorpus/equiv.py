"""Check each case's builder problem against its Verilator capture.

The builder problem is written by hand from the SV source; the capture is
what Verilator 5.052 made of the same source. z3 must find no assignment on
which one holds and the other does not (hard constraints only: `dist`
weights and `solve ... before` change the spread, not the solutions). Soft
constraints count as hard on both sides: Verilator's first query asserts
them, and relaxes them only when that query is unsat.

Usage: python -m tests.perf.svcorpus.equiv [NAME...]
"""
from __future__ import annotations

import subprocess
import sys

from tests.formal.soundness.ir import smt
from tests.perf.svcorpus import case as C
from tests.perf.svcorpus.vlt import problem_smt2_query


def _rename(t, m):
    if t[0] == "var":
        return ("var", m[t[1]], t[2])
    if t[0] in ("const", "true", "false"):
        return t
    head = 4 if t[0] == "extract" else 2
    return tuple(t[:head]) + tuple(_rename(a, m) for a in t[head:])


def script(case) -> str:
    from dv_solve.oracle import problem_smt2
    nw, ncons = problem_smt2_query(problem_smt2(C.problem(case, soft_as_hard=True)))
    cw, ccons = C.capture(case)
    ids = C.ids(case)
    m = {n: f"v{ids[n]}" for n in cw}
    for n, w in cw.items():
        if nw.get(m[n], w) != w:
            raise ValueError(f"{case.NAME}: {n} is {w} bits in the capture, "
                             f"{nw[m[n]]} in the case")
    ccons = [_rename(t, m) for t in ccons]
    widths = dict(nw)
    widths.update({m[n]: w for n, w in cw.items()})
    conj = lambda cs: "(and true " + " ".join(smt(t) for t in cs) + ")"   # noqa: E731
    decls = "".join(f"(declare-const {n} (_ BitVec {w}))" for n, w in sorted(widths.items()))
    return (f"(set-logic QF_BV){decls}(assert (not (= {conj(ncons)} {conj(ccons)})))"
            "(check-sat)(get-model)")


def box_script(case) -> str | None:
    """Solutions of the capture outside the case's BOX: must be none."""
    box = getattr(case, "BOX", {})
    if not box:
        return None
    cw, ccons = C.capture(case)
    decls = "".join(f"(declare-const {n} (_ BitVec {w}))" for n, w in sorted(cw.items()))
    out = " ".join(f"(bvult {n} (_ bv{lo} {cw[n]})) (bvugt {n} (_ bv{hi} {cw[n]}))"
                   for n, (lo, hi) in box.items())
    return (f"(set-logic QF_BV){decls}" + "".join(f"(assert {smt(t)})" for t in ccons)
            + f"(assert (or false {out}))(check-sat)(get-model)")


def _z3_run(text, timeout):
    from tests.formal.soundness.ir import _z3
    r = subprocess.run([_z3(), "-in", f"-T:{timeout}"], input=text,
                       capture_output=True, text=True)
    out = r.stdout.strip()
    first = out.split("\n", 1)[0] if out else r.stderr.strip()[:200]
    return first == "unsat", first if first != "sat" else out


def check(name: str, timeout: int = 120) -> tuple:
    case = C.load(name)
    ok, why = _z3_run(script(case), timeout)
    if ok and box_script(case):
        ok, why = _z3_run(box_script(case), timeout)
        why = "" if ok else f"a solution outside BOX: {why}"
    return ok, why


def main(argv) -> int:
    bad = 0
    for name in argv or C.names():
        ok, why = check(name)
        print(f"{'EQUIV' if ok else 'DIFF '} {name}" + ("" if ok else f": {why[:400]}"))
        bad += not ok
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
