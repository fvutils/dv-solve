"""Run-to-run noise of a SAT suite: sets the regression threshold (plan piece 0.6).

    python3 -m tests.perf.noise --suite fwhdl-core --split dev --repeats 10 \\
        [--budget 60] [--workers 4] [--out tests/perf/history/noise/<machine>.json]

Runs dv-solve's own arm (build/dv-solve-smt2, or --head-bin) over the suite
`repeats` times, one rep each, and reports for every category how far its
PAR-2 moves between runs of the same build: the median and the 95th
percentile of |PAR-2_i / PAR-2_j - 1| over all pairs of runs. A per-PR
regression gate (plan §3.4) should not fire inside that band; the
recommended threshold is the largest category p95, rounded up to a whole
percent, and never below 5 %.

Per-instance spread (coefficient of variation of CPU time, for instances
solved in every run) is reported too: instances above 20 % are the ones a
per-instance comparison cannot trust.
"""
from __future__ import annotations

import argparse
import datetime
import itertools
import json
import math
import statistics
import sys
from pathlib import Path

from . import builds, calib, run_sat, suites


def _pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return None
    k = (len(xs) - 1) * p
    lo, hi = math.floor(k), math.ceil(k)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def measure(spec, head, repeats, workers):
    arms = [run_sat.Arm("dv-smt2", "head", [str(head)])]
    spec = dict(spec, reps=1, reps_slow=1)
    runs = []
    for i in range(repeats):
        res = run_sat.run_suite(spec, arms, workers)
        runs.append({r["fixture"]: r for r in res["rows"]})
        print(f"  run {i + 1}/{repeats}", file=sys.stderr)
    return runs


def summarise(spec, runs):
    b = spec["budget_s"]
    cats = {}
    for fx in spec["fixtures"]:
        cats.setdefault(fx["cat"], []).append(fx["path"])

    def t(r):
        ok = r and r["verdict"] in ("sat", "unsat") and r["cpu_ms_min"] is not None
        return r["cpu_ms_min"] / 1000 if ok and r["cpu_ms_min"] / 1000 <= b else None

    out = {"categories": {}, "instances": {}}
    for c, fxs in list(cats.items()) + [("all", [f["path"] for f in spec["fixtures"]])]:
        par2 = [sum(t(run.get(f)) if t(run.get(f)) is not None else 2 * b for f in fxs)
                for run in runs]
        rel = [abs(x / y - 1) for x, y in itertools.permutations(par2, 2) if y > 0]
        out["categories"][c] = {"par2": [round(x, 3) for x in par2],
                                "median_rel": round(statistics.median(rel), 4) if rel else None,
                                "p95_rel": round(_pct(rel, 0.95), 4) if rel else None}
    cvs = {}
    for f in (fx["path"] for fx in spec["fixtures"]):
        ts = [t(run.get(f)) for run in runs]
        if all(x is not None for x in ts) and statistics.mean(ts) > 0:
            cvs[f] = statistics.pstdev(ts) / statistics.mean(ts)
    out["instances"] = {"solved_every_run": len(cvs),
                        "cv_median": round(statistics.median(cvs.values()), 4) if cvs else None,
                        "cv_p95": round(_pct(list(cvs.values()), 0.95), 4) if cvs else None,
                        "over_20pct": sorted(Path(f).stem for f, v in cvs.items() if v > 0.2)}
    worst = max((v["p95_rel"] or 0) for k, v in out["categories"].items() if k != "all")
    out["threshold_pct"] = max(5, math.ceil(worst * 100))
    return out


def markdown(spec, res, repeats):
    L = [f"Noise of `{spec['suite']}` over {repeats} runs of the same build, budget "
         f"{spec['budget_s']:g} s.", "",
         "| Category | PAR-2 per run (s) | median pairwise change | p95 pairwise change |",
         "|---|---|---|---|"]
    for c, v in res["categories"].items():
        L.append(f"| {c} | {min(v['par2']):.2f}–{max(v['par2']):.2f} | "
                 f"{(v['median_rel'] or 0) * 100:.1f} % | {(v['p95_rel'] or 0) * 100:.1f} % |")
    i = res["instances"]
    L += ["", f"Instances solved in every run: {i['solved_every_run']}; CPU-time coefficient of "
          f"variation median {(i['cv_median'] or 0) * 100:.1f} %, p95 {(i['cv_p95'] or 0) * 100:.1f} %.",
          f"Recommended regression threshold: **{res['threshold_pct']} %** of a category's PAR-2."]
    return "\n".join(L)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--suite", default="fwhdl-core")
    ap.add_argument("--split", default="dev")
    ap.add_argument("--repeats", type=int, default=10)
    ap.add_argument("--budget", type=float)
    ap.add_argument("--workers", type=int, default=0)
    ap.add_argument("--head-bin")
    ap.add_argument("--out", help="write the result as JSON here")
    a = ap.parse_args(argv)
    spec = suites.load(a.suite, a.split)
    if a.budget:
        spec["budget_s"] = a.budget
    runs = measure(spec, builds.head_bin(a.head_bin), a.repeats, a.workers)
    res = summarise(spec, runs)
    res.update(suite=spec["suite"], manifest=spec["hash"], repeats=a.repeats,
               budget_s=spec["budget_s"], machine=calib.machine(),
               utc=datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    print(markdown(spec, res, a.repeats))
    if a.out:
        Path(a.out).parent.mkdir(parents=True, exist_ok=True)
        Path(a.out).write_text(json.dumps(res, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
