#!/usr/bin/env python3
"""Run the SMT-COMP 2025 parallel-track bit-vector corpus and score against the
official competition reference numbers.

This corpus is the *parallel* track's bit-vector family (QF_BV / QF_ABV /
QF_UFBV / QF_AUFBV) -- 93 benchmarks hand-selected by the SMT-COMP organizers
as hard/unsolved-for-sequential instances. Many originate from yosys/SymbiYosys
hardware BMC (e.g. the ``2019-Wolf-fmbench`` VexRiscv-unrolled family), so they
are directly on-target for dv-solve.

Reference solver numbers (Bitwuzla, STP-Parti-Bitwuzla, Yices2) come straight
from the competition's published per-benchmark results and live in
``meta/manifest.json`` / ``meta/reference_results.csv``. Ground-truth sat/unsat
status (where known) comes from the SMT-LIB benchmark index.

IMPORTANT: the competition ran with a 1200s wall-clock limit on a large
multi-core machine (parallel track). A local single-thread run with a small
timeout is *not* wall-time comparable -- the meaningful comparison here is
solved-count and correctness (no wrong answers vs ground truth). Use a large
timeout and/or --logic QF_BV to reproduce the headline division.

Usage:
    direnv exec . python run_corpus.py --timeout 60 --logic QF_BV
    direnv exec . python run_corpus.py --timeout 1200 --solvers dv-solve-smt2,bitwuzla
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
# harness/ lives two levels up under tests/formal/
sys.path.insert(0, str(HERE.parents[1]))

from harness.dv_solve_smt2_solver import DvSolveSMT2Solver, DvSolveSMT2BBSolver  # noqa: E402
from harness.bitwuzla_solver import BitwuzlaSolver  # noqa: E402
from harness.z3_solver import Z3Solver  # noqa: E402


def load_manifest() -> list[dict]:
    return json.load(open(HERE / "meta" / "manifest.json"))


def local_path(entry: dict) -> Path:
    """Where the extracted .smt2 lives under smt2/<logic>/<family>/<name>."""
    return HERE / "smt2" / entry["logic"] / entry["family"] / entry["name"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="per-benchmark wall-clock timeout in seconds (competition used 1200)")
    ap.add_argument("--logic", action="append",
                    help="restrict to logic(s): QF_BV QF_ABV QF_UFBV QF_AUFBV (repeatable)")
    ap.add_argument("--filter", default=None, help="substring filter on benchmark name")
    ap.add_argument("--names", default=None,
                    help="comma-separated exact benchmark names to run (overrides --logic/--filter selection)")
    ap.add_argument("--solvers", default="dv-solve-smt2",
                    help="comma list from: dv-solve-smt2,dv-solve-smt2-bb,bitwuzla,z3")
    ap.add_argument("--limit", type=int, default=None, help="cap number of benchmarks (for smoke runs)")
    ap.add_argument("--out", default=str(HERE / "results" / "run.csv"))
    args = ap.parse_args()

    registry = {
        "dv-solve-smt2": DvSolveSMT2Solver(),
        "dv-solve-smt2-bb": DvSolveSMT2BBSolver(),
        "bitwuzla": BitwuzlaSolver(),
        "z3": Z3Solver(),
    }
    solvers = []
    for name in args.solvers.split(","):
        s = registry.get(name.strip())
        if s is None:
            print(f"[warn] unknown solver {name!r}", file=sys.stderr)
            continue
        if s.is_available():
            solvers.append(s)
        else:
            print(f"[warn] {name} not available -- skipping", file=sys.stderr)
    if not solvers:
        print("no available solvers selected", file=sys.stderr)
        return 2

    manifest = load_manifest()
    wanted_logics = set(args.logic) if args.logic else None
    wanted_names = set(args.names.split(",")) if args.names else None
    bench = []
    for e in manifest:
        if wanted_names is not None:
            if e["name"] not in wanted_names:
                continue
        else:
            if wanted_logics and e["logic"] not in wanted_logics:
                continue
            if args.filter and args.filter not in e["name"]:
                continue
        p = local_path(e)
        if not p.is_file():
            print(f"[warn] missing file (run extract first): {p}", file=sys.stderr)
            continue
        bench.append((e, p))
    if args.limit:
        bench = bench[: args.limit]
    if not bench:
        print("no benchmarks matched (did you run extract_from_archive.py?)", file=sys.stderr)
        return 2

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    tallies = {s.name: {"solved": 0, "correct": 0, "wrong": 0, "unknown": 0, "timeout": 0}
               for s in solvers}
    fieldnames = ["logic", "family", "name", "truth", "solver", "result", "wall_s",
                  "bitwuzla_ref", "stp_parti_ref"]
    # write incrementally so a background interruption keeps partial results
    out_fh = open(args.out, "w", newline="")
    writer = csv.DictWriter(out_fh, fieldnames=fieldnames)
    writer.writeheader()
    out_fh.flush()

    for i, (e, p) in enumerate(bench, 1):
        truth = e["status"]  # sat / unsat / unknown
        print(f"[{i}/{len(bench)}] {e['logic']}/{e['name']}  (truth={truth})", flush=True)
        for s in solvers:
            t0 = time.time()
            r = s.solve(p, timeout_s=args.timeout)
            dt = time.time() - t0
            got = r.result  # sat/unsat/unknown/timeout/error
            t = tallies[s.name]
            if got in ("sat", "unsat"):
                t["solved"] += 1
                if truth in ("sat", "unsat"):
                    if got == truth:
                        t["correct"] += 1
                    else:
                        t["wrong"] += 1
                        print(f"    !!! {s.name} WRONG: got {got}, truth {truth}")
            elif got == "timeout":
                t["timeout"] += 1
            else:
                t["unknown"] += 1
            print(f"    {s.name:22s} {got:8s} {dt:8.2f}s", flush=True)
            writer.writerow(dict(logic=e["logic"], family=e["family"], name=e["name"],
                                 truth=truth, solver=s.name, result=got,
                                 wall_s=round(dt, 3),
                                 bitwuzla_ref=e["solvers"].get("Bitwuzla", {}).get("result", ""),
                                 stp_parti_ref=e["solvers"].get("STP-Parti-Bitwuzla", {}).get("result", "")))
            out_fh.flush()
    out_fh.close()

    # summary vs reference
    print("\n===== SUMMARY (timeout=%.0fs, %d benchmarks) =====" % (args.timeout, len(bench)))
    for name, t in tallies.items():
        print(f"  {name:22s} solved {t['solved']:3d}  correct {t['correct']:3d}  "
              f"wrong {t['wrong']:3d}  timeout {t['timeout']:3d}  other {t['unknown']:3d}")
    # reference (competition, 1200s, multi-core)
    ref = {}
    for e, _ in bench:
        for sv, d in e["solvers"].items():
            ref.setdefault(sv, 0)
            if d.get("result") in ("sat", "unsat"):
                ref[sv] += 1
    print("  --- competition reference (1200s, parallel HW) ---")
    for sv, n in sorted(ref.items()):
        print(f"  {sv:22s} solved {n:3d}")
    print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
