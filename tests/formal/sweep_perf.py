"""Perf sweep: dv-solve vs bitwuzla on the 'simpler' fixture sets.

For every fixture in the chosen tiers, measure min-of-N wall time (product
level: startup+parse+encode+solve) for:
  * bitwuzla
  * dv-solve default engine (auto/cdcl routing)
  * dv-solve bitblast engine
Also capture the dv bitblast encode-ms / sat-ms split (DV_BB_STATS) and a few
static features per fixture. Emits CSV to stdout-designated --out.

Usage: direnv exec . python tests/formal/sweep_perf.py --out results/sweep.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DV = REPO / "build" / "dv-solve-smt2"
BWZ = REPO / "packages" / "verilator-bin" / "bin" / "bitwuzla"

SETS = {
    "tier1": HERE / "smt2" / "tier1",
    "tier2": HERE / "smt2" / "tier2",
    "tier3": HERE / "smt2" / "tier3",
    "gaps": HERE / "smt2" / "gaps",
    "verilator": HERE / "smt2" / "verilator",
    "wide": HERE / "smt2" / "wide",
}

BB_RE = re.compile(r"bb=([\d.]+)ms sat=([\d.]+)ms ands=(\d+) vars=(\d+) clauses=(\d+)")


def parse_res(out: str) -> str:
    for line in out.splitlines():
        t = line.strip()
        if t in ("sat", "unsat", "unknown"):
            return t
    return "unknown"


def run(argv, env=None, timeout=30.0):
    e = None
    if env:
        e = os.environ.copy()
        e.update(env)
    t0 = time.monotonic()
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, env=e)
    except subprocess.TimeoutExpired:
        return "timeout", (time.monotonic() - t0) * 1000.0, ""
    dt = (time.monotonic() - t0) * 1000.0
    res = parse_res(p.stdout)
    if p.returncode != 0 and not p.stdout.strip():
        res = "error"
    return res, dt, p.stderr


def min_wall(argv, env=None, reps=5, timeout=30.0):
    best = None
    res = None
    for _ in range(reps):
        r, dt, _ = run(argv, env=env, timeout=timeout)
        res = r
        if best is None or dt < best:
            best = dt
        if r in ("timeout", "error"):
            break
    return res, best


def bb_split(path):
    """Return (encode_ms, sat_ms, ands, vars, clauses) from DV_BB_STATS, or Nones."""
    _, _, stderr = run([str(DV), str(path)], env={"DV_ENGINE": "bitblast", "DV_BB_STATS": "1"})
    m = BB_RE.search(stderr)
    if not m:
        return (None,) * 5
    return (float(m.group(1)), float(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))


def features(path):
    txt = path.read_text()
    return {
        "bytes": len(txt),
        "asserts": txt.count("(assert"),
        "arrays": txt.count("declare-fun") + txt.count("Array") if "Array" in txt else txt.count("declare-fun"),
        "has_array": "Array" in txt,
        "select": txt.count("select"),
        "store": txt.count("store"),
        "bvmul": txt.count("bvmul"),
        "bvudiv": txt.count("bvudiv") + txt.count("bvurem"),
        "bvsdiv": txt.count("bvsdiv") + txt.count("bvsrem") + txt.count("bvsmod"),
        "ite": txt.count("(ite"),
        "concat": txt.count("concat"),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(HERE / "results" / "sweep.csv"))
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--sets", default=None, help="comma list; default all")
    args = ap.parse_args()

    wanted = args.sets.split(",") if args.sets else list(SETS)
    rows = []
    for sname in wanted:
        d = SETS[sname]
        for fx in sorted(d.glob("*.smt2")):
            f = features(fx)
            bwz_res, bwz_ms = min_wall([str(BWZ), str(fx)], reps=args.reps, timeout=args.timeout)
            dv_res, dv_ms = min_wall([str(DV), str(fx)], reps=args.reps, timeout=args.timeout)
            bb_res, bb_ms = min_wall([str(DV), str(fx)], env={"DV_ENGINE": "bitblast"},
                                     reps=args.reps, timeout=args.timeout)
            enc, sat, ands, nvars, ncl = bb_split(fx)
            # best dv wall across engines
            dv_best = min(dv_ms, bb_ms)
            ratio = dv_best / bwz_ms if bwz_ms else 0
            row = {
                "set": sname, "bench": fx.stem,
                "bwz_res": bwz_res, "bwz_ms": round(bwz_ms, 2),
                "dv_res": dv_res, "dv_ms": round(dv_ms, 2),
                "bb_res": bb_res, "bb_ms": round(bb_ms, 2),
                "dv_best_ms": round(dv_best, 2), "ratio_dvbest_bwz": round(ratio, 2),
                "enc_ms": enc, "sat_ms": sat, "ands": ands, "vars": nvars, "clauses": ncl,
                **f,
            }
            rows.append(row)
            print(f"{sname:<10}{fx.stem:<34} bwz={bwz_ms:7.2f}({bwz_res:<6}) "
                  f"dv={dv_ms:7.2f}({dv_res:<6}) bb={bb_ms:7.2f}({bb_res:<6}) "
                  f"ratio={ratio:5.2f}", flush=True)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"\nwrote {out} ({len(rows)} rows)")

    # disagreements
    print("\n== disagreements (definitive sat/unsat mismatch) ==")
    n = 0
    for r in rows:
        defs = {x for x in (r["bwz_res"], r["dv_res"], r["bb_res"]) if x in ("sat", "unsat")}
        if len(defs) > 1:
            print(f"  DISAGREE {r['bench']}: bwz={r['bwz_res']} dv={r['dv_res']} bb={r['bb_res']}")
            n += 1
    print(f"({n})")


if __name__ == "__main__":
    main()
