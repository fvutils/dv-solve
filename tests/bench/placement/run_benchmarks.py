#!/usr/bin/env python3
"""Run placement benchmarks and collect results.

Usage:
    python run_benchmarks.py                    # Generate + run all
    python run_benchmarks.py --suite tier_a     # Run only Tier A
    python run_benchmarks.py --bench-dir path/  # Run existing benchmarks
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

# Ensure project root is on sys.path
_ROOT = Path(__file__).parent.parent.parent.parent
if str(_ROOT / "src") not in sys.path:
    sys.path.insert(0, str(_ROOT / "src"))

from generate_benchmarks import generate_all_suites, generate_suite, ALL_SUITES
from placement_harness import run_benchmark, print_results_table, PlacementResult


def main():
    parser = argparse.ArgumentParser(description="Run placement benchmarks")
    parser.add_argument("--suite", choices=list(ALL_SUITES) + ["all"],
                        default="all")
    parser.add_argument("--bench-dir", type=Path, default=None,
                        help="Directory with pre-generated JSON benchmarks")
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).parent / "benchdata")
    parser.add_argument("--time-budget", type=float, default=60.0,
                        help="Time budget per benchmark (seconds)")
    parser.add_argument("--results-file", type=Path, default=None,
                        help="Write results to JSON file")
    args = parser.parse_args()

    # Set up library path
    build_dir = _ROOT / "build"
    if build_dir.exists():
        os.environ["ZSP_SOLVER_PATH"] = str(build_dir)

    # Generate benchmarks if needed
    if args.bench_dir is None:
        print("Generating benchmark data...")
        if args.suite == "all":
            paths = generate_all_suites(args.output_dir)
        else:
            paths = generate_suite(args.suite, args.output_dir / args.suite)
        bench_dirs = set(p.parent for p in paths)
    else:
        bench_dirs = {args.bench_dir}

    # Run benchmarks
    all_results = []
    for bd in sorted(bench_dirs):
        suite_name = bd.name
        print(f"\n=== Suite: {suite_name} ===")
        bench_files = sorted(bd.glob("*.json"))
        if not bench_files:
            print(f"  No benchmark files in {bd}")
            continue

        for bf in bench_files:
            print(f"Running {bf.name}...")
            t0 = time.monotonic()
            try:
                r = run_benchmark(bf, args.time_budget)
                status = "FEASIBLE" if r.feasible else f"result={r.solver_result}"
                print(f"  {r.name}: {status}, HPWL={r.hpwl}, "
                      f"time={r.t_first_sec:.3f}s")
                all_results.append(r)
            except Exception as e:
                print(f"  ERROR: {e}")
                all_results.append(PlacementResult(name=bf.stem))

    # Print summary
    if all_results:
        print_results_table(all_results)

    # Write results
    if args.results_file:
        results_json = []
        for r in all_results:
            results_json.append({
                "name": r.name,
                "n_macros": r.n_macros,
                "n_nets": r.n_nets,
                "feasible": r.feasible,
                "hpwl": r.hpwl,
                "t_first_sec": r.t_first_sec,
                "t_total_sec": r.t_total_sec,
                "solver_result": r.solver_result,
            })
        args.results_file.parent.mkdir(parents=True, exist_ok=True)
        with open(args.results_file, "w") as f:
            json.dump(results_json, f, indent=2)
        print(f"\nResults written to {args.results_file}")


if __name__ == "__main__":
    main()
