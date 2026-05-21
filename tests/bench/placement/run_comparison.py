#!/usr/bin/env python3
"""Run placement benchmarks comparing CostGuided ON vs OFF.

Produces a side-by-side results table and writes JSON results.

Usage:
    python run_comparison.py                       # Run Tier A only
    python run_comparison.py --suite all            # Run all tiers
    python run_comparison.py --suite tier_a tier_b  # Run specific tiers
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

_ROOT = Path(__file__).parent.parent.parent.parent
if str(_ROOT / "src") not in sys.path:
    sys.path.insert(0, str(_ROOT / "src"))

sys.path.insert(0, str(Path(__file__).parent))
from generate_benchmarks import generate_all_suites, generate_suite, ALL_SUITES
from placement_harness import run_benchmark, PlacementResult


def run_comparison(bench_path: Path, time_budget: float = 30.0):
    """Run one benchmark with CostGuided OFF and ON, return both results."""
    r_off = run_benchmark(bench_path, time_budget, use_cost_guided=False)
    r_on  = run_benchmark(bench_path, time_budget, use_cost_guided=True)
    return r_off, r_on


def main():
    parser = argparse.ArgumentParser(description="CostGuided comparison")
    parser.add_argument("--suite", nargs="+",
                        choices=list(ALL_SUITES) + ["all"],
                        default=["tier_a"])
    parser.add_argument("--bench-dir", type=Path, default=None)
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).parent / "benchdata")
    parser.add_argument("--time-budget", type=float, default=30.0)
    parser.add_argument("--results-file", type=Path,
                        default=Path(__file__).parent / "benchdata" / "comparison.json")
    args = parser.parse_args()

    build_dir = _ROOT / "build"
    if build_dir.exists():
        os.environ["ZSP_SOLVER_PATH"] = str(build_dir)

    # Determine which suites to run
    suites = args.suite
    if "all" in suites:
        suites = list(ALL_SUITES.keys())

    # Collect benchmark files
    bench_files = []
    for suite_name in suites:
        suite_dir = args.output_dir / suite_name
        if not suite_dir.exists():
            print(f"Generating {suite_name}...")
            generate_suite(suite_name, suite_dir)
        for f in sorted(suite_dir.glob("*.json")):
            bench_files.append((suite_name, f))

    # Run comparisons
    all_results = []
    print(f"\n{'Bench':<6} {'N':>4} {'Nets':>5}  "
          f"{'--- Random ---':^26}  {'--- CostGuided ---':^26}  "
          f"{'HPWL':>6}")
    print(f"{'':6} {'':4} {'':5}  "
          f"{'Feas':>5} {'HPWL':>8} {'Time(s)':>10}  "
          f"{'Feas':>5} {'HPWL':>8} {'Time(s)':>10}  "
          f"{'Impr%':>6}")
    print("-" * 95)

    for suite_name, bf in bench_files:
        print(f"  Running {bf.stem}...", end="", flush=True)
        t0 = time.monotonic()
        try:
            r_off, r_on = run_comparison(bf, args.time_budget)
        except Exception as e:
            print(f" ERROR: {e}")
            continue
        dt = time.monotonic() - t0

        hpwl_off = r_off.hpwl if r_off.feasible else -1
        hpwl_on  = r_on.hpwl  if r_on.feasible  else -1
        if hpwl_off > 0 and hpwl_on > 0:
            impr = (1 - hpwl_on / hpwl_off) * 100
            impr_str = f"{impr:+.1f}%"
        else:
            impr_str = "-"

        feas_off = "YES" if r_off.feasible else "NO"
        feas_on  = "YES" if r_on.feasible  else "NO"
        hpwl_off_s = str(hpwl_off) if r_off.feasible else "-"
        hpwl_on_s  = str(hpwl_on)  if r_on.feasible  else "-"

        print(f"\r{bf.stem:<6} {r_off.n_macros:>4} {r_off.n_nets:>5}  "
              f"{feas_off:>5} {hpwl_off_s:>8} {r_off.t_first_sec:>10.3f}  "
              f"{feas_on:>5} {hpwl_on_s:>8} {r_on.t_first_sec:>10.3f}  "
              f"{impr_str:>6}")

        all_results.append({
            "name": bf.stem,
            "suite": suite_name,
            "n_macros": r_off.n_macros,
            "n_nets": r_off.n_nets,
            "random": {
                "feasible": r_off.feasible,
                "hpwl": r_off.hpwl,
                "time_sec": round(r_off.t_first_sec, 4),
                "solver_result": r_off.solver_result,
            },
            "cost_guided": {
                "feasible": r_on.feasible,
                "hpwl": r_on.hpwl,
                "time_sec": round(r_on.t_first_sec, 4),
                "solver_result": r_on.solver_result,
            },
        })

    # Write results
    if args.results_file and all_results:
        args.results_file.parent.mkdir(parents=True, exist_ok=True)
        with open(args.results_file, "w") as f:
            json.dump(all_results, f, indent=2)
        print(f"\nResults written to {args.results_file}")


if __name__ == "__main__":
    main()
