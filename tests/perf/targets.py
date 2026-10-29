#!/usr/bin/env python3
"""Pass/fail of local simbench timings against per-bench local targets.

Target for a bench = best reference-config us/call (from a reference-host
simbench results jsonl) / --host-factor (reference host -> this host).
The hot number (ns_per_call) is judged; the cold first call is shown beside it.

  python3 tests/perf/targets.py --ref <ref.jsonl> --local <local.jsonl> [--all]

Reference data with commercial numbers is private (docs/notes, gitignored);
this script only reads it.
"""
import argparse, collections, json, math, sys

# Reference set (P0.3): worst engine-routing losses + tails + one easy bench.
REF_SET = [
    "arith.add_ovf", "arith.mod_align", "arith.unsigned_ops", "arith.mul_div_64",
    "dist.dist_mixed", "dist.randc_basic", "array.sorted_increasing",
    "array.unique_32", "hard.unique_64", "control.solve_before",
]


def load(path):
    d = collections.defaultdict(dict)
    for line in open(path):
        r = json.loads(line)
        if not r.get("valid", True):
            continue
        s = r["simbench"]
        d[s["config"]][s["bench"]] = s["result"]
    return d


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ref", default="docs/notes/simbench/trial2/data/results-latest.jsonl")
    ap.add_argument("--ref-configs", default="vcs,xcelium,questa")
    ap.add_argument("--local", default="docs/notes/simbench/2026-10-28-local/dvs-standalone.jsonl")
    ap.add_argument("--config", default="dvs-standalone")
    ap.add_argument("--host-factor", type=float, default=2.19)
    ap.add_argument("--all", action="store_true", help="every bench, not the reference set")
    a = ap.parse_args(argv)

    ref, loc = load(a.ref), load(a.local)[a.config]
    cfgs = [c for c in a.ref_configs.split(",") if c in ref]
    benches = sorted(loc) if a.all else REF_SET
    print(f"{'bench':30s} {'hot_us':>9s} {'cold_us':>9s} {'target':>9s} {'x':>6s}  result")
    fails = ratios = 0
    logs = []
    for b in benches:
        refs = [ref[c][b]["ns_per_call"] for c in cfgs if b in ref[c]]
        if b not in loc or not refs:
            print(f"{b:30s} {'-':>9s} {'-':>9s} {'-':>9s} {'-':>6s}  MISSING")
            fails += 1
            continue
        r = loc[b]
        hot, cold = r["ns_per_call"] / 1e3, r.get("first_call_ns", 0) / 1e3
        tgt = min(refs) / 1e3 / a.host_factor
        x = hot / tgt
        logs.append(math.log(x))
        ok = x <= 1.0 and not r.get("failures") and not r.get("timed_out")
        fails += not ok
        print(f"{b:30s} {hot:9.1f} {cold:9.1f} {tgt:9.1f} {x:6.2f}  {'PASS' if ok else 'FAIL'}")
    gm = math.exp(sum(logs) / len(logs)) if logs else float("nan")
    print(f"{len(benches) - fails}/{len(benches)} pass; geomean hot/target {gm:.2f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
