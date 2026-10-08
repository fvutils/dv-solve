"""Confirm the fwhdl-core `:status` values with the pinned competitors.

    python tests/formal/fwhdl_consensus.py [--budget 60] [--jobs 12] [--only REGEX]

gen_fwhdl.py derives each instance's status from its construction. This runs
every instance on z3, bitwuzla, boolector, cvc5 and yices (tests/perf/tools.py
pins them) and records their answers in gen/index.json under `consensus`
({solver: verdict}). Acceptance (plan piece 0.2): no solver contradicts the
construction, and at least two confirm it, unless no competitor solves the
instance within the budget (then it is a hard instance, and stays).

Exit status 1 if any solver contradicts a status; that is either a generator
bug or a competitor bug, and needs a look before the suite is used.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO))
from tests.perf import run_sat, tools  # noqa: E402

GEN = _REPO / "tests" / "formal" / "smt2" / "fw_hdl" / "gen"

# Competitor answers checked by hand and found wrong: (instance id, solver) -> why.
# Reported, not counted as contradicting the construction.
KNOWN_WRONG = {
    ("fnv1a_w64_bytes4", "yices"):
        "yices 2.6.5 answers sat with a = #x87ea0ec5; both FNV-1a forms give "
        "#xa11d2596da8be2a5 there (the generator's evaluator), and z3 and dv-solve "
        "answer unsat with that a pinned (2026-10-08)",
}


def argv(name: str, path) -> list:
    p = str(tools.path(name))
    return {"z3": [p, "-smt2"], "bitwuzla": [p], "boolector": [p, "--smt2"],
            "cvc5": [p], "yices": [p]}[name] + [str(path)]


def main(argv_=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--budget", type=float, default=60.0, help="seconds per solver run")
    ap.add_argument("--jobs", type=int, default=12)
    ap.add_argument("--only", help="regex on instance ids")
    ap.add_argument("--solvers", default=",".join(tools.SOLVERS))
    ap.add_argument("--redo", action="store_true", help="rerun instances that have consensus")
    a = ap.parse_args(argv_)
    solvers = a.solvers.split(",")
    tools.check(solvers)
    index_p = GEN / "index.json"
    index = json.loads(index_p.read_text())
    todo = [e for e in index["instances"]
            if (not a.only or re.search(a.only, e["id"]))
            and (a.redo or set(solvers) - set(e.get("consensus", {})))]
    jobs = [(e, s) for e in todo for s in solvers if a.redo or s not in e.get("consensus", {})]
    print(f"{len(jobs)} runs over {len(todo)} instances, budget {a.budget:g}s", file=sys.stderr)

    def one(job):
        e, s = job
        verdict, cpu, wall, _ = run_sat.run_once(argv(s, GEN / e["path"]), {}, a.budget)
        return e, s, verdict, round(wall / 1000, 3)

    done = 0
    with ThreadPoolExecutor(a.jobs) as ex:
        for e, s, verdict, secs in ex.map(one, jobs):
            e.setdefault("consensus", {})[s] = verdict
            e.setdefault("consensus_s", {})[s] = secs
            done += 1
            if done % 100 == 0:
                print(f"  {done}/{len(jobs)}", file=sys.stderr)
                index_p.write_text(json.dumps(index, indent=1) + "\n")
    index_p.write_text(json.dumps(index, indent=1) + "\n")
    return report(index)


def report(index) -> int:
    bad, weak, hard = [], [], []
    for e in index["instances"]:
        c = e.get("consensus", {})
        definite = {s: v for s, v in c.items() if v in ("sat", "unsat")}
        wrong = {s: v for s, v in definite.items()
                 if v != e["status"] and (e["id"], s) not in KNOWN_WRONG}
        if wrong:
            bad.append((e["id"], e["status"], wrong))
        elif not definite:
            hard.append(e["id"])
        elif len(definite) < 2:
            weak.append(e["id"])
    n = len(index["instances"])
    print(f"{n} instances: {n - len(bad) - len(weak) - len(hard)} confirmed by >= 2 solvers, "
          f"{len(weak)} by one, {len(hard)} by none within the budget, "
          f"{len(bad)} contradicted")
    for (i, s), why in KNOWN_WRONG.items():
        print(f"  known competitor defect, {s} on {i}: {why}")
    for i, st, w in bad:
        print(f"  CONTRADICTED {i}: status {st}, {w}")
    for i in weak:
        print(f"  one solver only: {i}")
    for i in hard:
        print(f"  no solver: {i}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
