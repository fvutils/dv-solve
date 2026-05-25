"""Benchmark dv-solve against external SMT solvers on tier2 + tier3 fixtures.

Runs every .smt2 fixture in tier2/ and tier3/ through dv-solve, z3, and
boolector. Reports correctness (do the answers agree?) and timing.

Usage:
    direnv exec . python tests/formal/run_compare.py [--timeout S] [--tier N]
"""
from __future__ import annotations

import argparse
import csv
import sys
import time
from pathlib import Path

from harness.dv_solve_smt2_solver import DvSolveSMT2Solver
from harness.z3_solver import Z3Solver
from harness.boolector_solver import BoolectorSolver
from harness.bitwuzla_solver import BitwuzlaSolver


HERE = Path(__file__).resolve().parent
TIER_DIRS = {
    1: HERE / "smt2" / "tier1",
    2: HERE / "smt2" / "tier2",
    3: HERE / "smt2" / "tier3",
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--tier", type=int, default=None, action="append", help="restrict to tier(s); repeatable")
    ap.add_argument("--out", default=str(HERE / "results" / "compare.csv"))
    ap.add_argument("--filter", default=None, help="substring filter on benchmark name")
    ap.add_argument("--solvers", default=None, help="comma-separated solver names to include")
    args = ap.parse_args()

    all_solvers = (DvSolveSMT2Solver(), Z3Solver(), BoolectorSolver(), BitwuzlaSolver())
    wanted = set(args.solvers.split(",")) if args.solvers else None
    solvers = []
    for s in all_solvers:
        if wanted is not None and s.name not in wanted:
            continue
        if s.is_available():
            solvers.append(s)
        else:
            print(f"[warn] {s.name} not available", file=sys.stderr)

    fixtures = []
    for tier, d in TIER_DIRS.items():
        if args.tier and tier not in args.tier:
            continue
        if d.is_dir():
            for f in sorted(d.glob("*.smt2")):
                if args.filter and args.filter not in f.stem:
                    continue
                fixtures.append((tier, f))

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    rows = []
    print(f"# {len(fixtures)} fixtures, {len(solvers)} solvers, timeout {args.timeout}s")
    hdr = f"{'tier':<5}{'benchmark':<40}"
    for s in solvers:
        hdr += f"{s.name+'_res':<18}{s.name+'_ms':<12}"
    print(hdr)

    for tier, fx in fixtures:
        row = {"tier": tier, "benchmark": fx.stem}
        line = f"{tier:<5}{fx.stem:<40}"
        for s in solvers:
            r = s.solve(fx, timeout_s=args.timeout)
            row[f"{s.name}_result"] = r.result
            row[f"{s.name}_ms"] = f"{r.solve_time_ms:.0f}"
            line += f"{r.result:<18}{r.solve_time_ms:<12.0f}"
        rows.append(row)
        print(line, flush=True)

    # CSV
    fieldnames = ["tier", "benchmark"]
    for s in solvers:
        fieldnames.append(f"{s.name}_result")
        fieldnames.append(f"{s.name}_ms")
    with out_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)

    # Summary
    print()
    print("== summary ==")
    for s in solvers:
        sat = sum(1 for r in rows if r[f"{s.name}_result"] == "sat")
        unsat = sum(1 for r in rows if r[f"{s.name}_result"] == "unsat")
        to = sum(1 for r in rows if r[f"{s.name}_result"] == "timeout")
        err = sum(1 for r in rows if r[f"{s.name}_result"] in ("error", "unknown"))
        total_ms = sum(float(r[f"{s.name}_ms"]) for r in rows)
        print(f"  {s.name:<20} sat={sat:<3} unsat={unsat:<3} timeout={to:<3} unknown/err={err:<3}  total={total_ms:.0f}ms")

    # Disagreements
    print()
    print("== disagreements ==")
    n_disagree = 0
    if len(solvers) >= 2:
        for r in rows:
            results = {s.name: r[f"{s.name}_result"] for s in solvers}
            definitive = {k: v for k, v in results.items() if v in ("sat", "unsat")}
            if len(set(definitive.values())) > 1:
                print(f"  DISAGREE  {r['benchmark']:<40} {results}")
                n_disagree += 1
    print(f"({n_disagree} disagreements)")

    print(f"\nwrote {out_path}")
    return 1 if n_disagree else 0


if __name__ == "__main__":
    sys.exit(main())
