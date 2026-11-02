"""Training data for the expected-cost model (dv_solve.perfmodel): our corpus
run under every arm we have, for the best case per problem.

    python -m tests.perf.perfmodel_corpus --build build -o train.json

Each corpus item runs on its own, so its classes are known:
  - **ours:** dv-solve as the suite runs it, with DV_STATS on (the class
    records, our cost, our time);
  - **other arms**, timed: for SMT-LIB2 fixtures (sat-core, fwhdl-core),
    dv-solve with each engine forced, and the reference solvers (z3,
    bitwuzla, boolector, cvc5, yices); for randomization items (svcorpus,
    rand-core, through the API), a plain solve without uniform sampling, and
    z3 and bitwuzla on the problem exported as SMT-LIB2 when our solve takes
    over REF_MIN_S (below that, process start-up swamps the solve).

An arm counts only with the same verdict as ours. Process times are the
median of REPS runs less the tool's start-up (measured on a trivial file),
and no less than NOISE_S: below it, start-up jitter swamps the solve, so a
problem that fast never shows a gap. Per class, `frontier` is the best
arm's time over ours (at most 1). For a randomization item that asked for
uniform sampling, a single solve is not the same service: its best case is
the best single solve times UNIFORM_ALLOWANCE, a stated policy (uniform
sampling should cost at most that), not something learned from our own cost.
docs/solve_stats_capture_plan.md, S5a.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(_REPO), str(_REPO / "src")]

UNIFORM_ALLOWANCE = 4.0     # policy: a uniform draw may cost this many best single solves
REF_MIN_S = 1e-3            # references only when our solve takes longer than this
NOISE_S = 2e-3              # process times below this are start-up noise: clamped to it
REPS = 3                    # runs per process arm; the median counts
SMT2_REFS = ("z3", "bitwuzla", "boolector", "cvc5", "yices")
RAND_REFS = ("z3", "bitwuzla")
DV_ENGINES = ("cdcl", "bitblast", "cube")
_TRIVIAL = "(set-logic QF_BV)\n(declare-const x (_ BitVec 8))\n(assert (= x #x01))\n(check-sat)\n"

# A randomization item, one process: its problem, N solves, per-solve times.
# kind "rand": a rand-core benchmark, uniform as the suite runs it;
# kind "sv": an svcorpus case, a seeded solve as its bench does.
_WORKER = r"""
import json, random, sys, time
sys.path[:0] = [{repo!r}, {repo!r} + "/src"]
kind, name, n, fair, smt2_out = {kind!r}, {name!r}, {n}, {fair}, {smt2!r}
from dv_solve.ctx import SolveCtx, SOLVE_OK
if kind == "rand":
    from tests.perf import rand_benches
    from tests.formal.soundness.doors import _lower
    from dv_solve.builder import SolveProblemBuilder
    b = rand_benches.Bench(name)
    ids = {{k: i for i, k in enumerate(b.names)}}
    bld = SolveProblemBuilder()
    for k in b.names:
        bld.add_var(ids[k], width=b.widths[k], is_signed=False, lo=0, hi=(1 << b.widths[k]) - 1)
    for c in b.cons:
        bld.add_constraint(_lower(bld, c, ids))
    prob, _ = bld.finalize()
else:
    from tests.perf.svcorpus import case as C
    prob = C.problem(C.load(name))
if smt2_out:
    from dv_solve.oracle import problem_smt2
    open(smt2_out, "w").write(problem_smt2(prob))
rng = random.Random(1)
ts, sat = [], 0
with SolveCtx(prob) as ctx:
    for _ in range(n):
        ctx.reset()
        t = time.perf_counter()
        rc = ctx.solve(seed=rng.getrandbits(62) + 1, fair_pick=fair)
        ts.append(time.perf_counter() - t)
        sat += rc == SOLVE_OK
print(json.dumps({{"t": sorted(ts)[len(ts) // 2], "sat": sat, "n": n}}))
"""


def _env(build: Path, stats_dir=None) -> dict:
    env = dict(os.environ)
    env.pop("LD_LIBRARY_PATH", None)
    env.pop("DV_STATS", None)
    env["DVS_SOLVER_PATH"] = str(build / "lib")
    env["PYTHONPATH"] = os.pathsep.join([str(_REPO / "src"), str(_REPO)])
    if stats_dir is not None:
        env["DV_STATS"] = str(Path(stats_dir) / "s.%p.json")
    return env


def _stats_of(d: Path):
    from dv_solve import stats
    files = [stats.load(p) for p in glob.glob(str(d / "*.json"))]
    return stats.merge(files) if files else None


def _startup(argv, env, run_once, budget) -> float:
    with tempfile.NamedTemporaryFile("w", suffix=".smt2", delete=False) as f:
        f.write(_TRIVIAL)
    try:
        return statistics.median(run_once(argv + [f.name], env, budget)[2] / 1e3
                                 for _ in range(5))
    finally:
        os.unlink(f.name)


def _timed(argv, env, budget, run_once, startup, want=None):
    """(verdict, net seconds): the median of REPS runs, less start-up, at
    least NOISE_S; verdict None if a run disagrees with `want` or fails."""
    walls = []
    v0 = None
    for _ in range(REPS):
        v, _, w, _ = run_once(argv, env, budget)
        if v not in ("sat", "unsat") or (want and v != want) or (v0 and v != v0):
            return None, None
        v0 = v
        walls.append(w / 1e3)
        if w / 1e3 >= budget * 0.5:         # slow: one run is enough
            break
    return v0, max(statistics.median(walls) - startup, NOISE_S)


def _smt2_item(path, build, refs, startup, budget, run_once):
    """(stats, our time, {arm: time}) for one SMT-LIB2 fixture."""
    smt2 = str(build / "dv-solve-smt2")
    with tempfile.TemporaryDirectory(prefix="pm-") as td:
        v, _, _, _ = run_once([smt2, path], _env(build, td), budget)    # the stats run
        st = _stats_of(Path(td))
    if v not in ("sat", "unsat") or st is None:
        return None
    _, ours = _timed([smt2, path], _env(build), budget, run_once, startup["dv"], v)
    if ours is None:
        return None
    arms = {}
    for e in DV_ENGINES:
        _, te = _timed([smt2, path], {**_env(build), "DV_ENGINE": e}, budget, run_once,
                       startup["dv"], v)
        if te is not None:
            arms[f"dv-{e}"] = te
    for name, argv in refs.items():
        _, tr = _timed(argv + [path], _env(build), budget, run_once, startup[name], v)
        if tr is not None:
            arms[name] = tr
    return st, ours, arms


def _rand_item(kind, name, build, refs, startup, budget, run_once, n):
    """(stats, our time, {arm: time}, uniform) for one randomization item."""
    fair = kind == "rand"
    with tempfile.TemporaryDirectory(prefix="pm-") as td:
        smt2 = str(Path(td) / "p.smt2")
        code = _WORKER.format(repo=str(_REPO), kind=kind, name=name, n=n, fair=fair, smt2=smt2)
        sd = Path(td) / "stats"
        sd.mkdir()
        r = subprocess.run([sys.executable, "-c", code], env=_env(build, sd), cwd=_REPO,
                           capture_output=True, text=True, timeout=budget * 10)
        st = _stats_of(sd)
        if r.returncode != 0 or st is None:
            return None
        ours = json.loads(r.stdout.splitlines()[-1])["t"]
        arms = {}
        if fair:            # a plain solve: the single-solve lower bound
            code = _WORKER.format(repo=str(_REPO), kind=kind, name=name, n=n, fair=False,
                                  smt2="")
            r2 = subprocess.run([sys.executable, "-c", code], env=_env(build), cwd=_REPO,
                                capture_output=True, text=True, timeout=budget * 10)
            if r2.returncode == 0:
                arms["dv-single"] = json.loads(r2.stdout.splitlines()[-1])["t"]
        if ours > REF_MIN_S:
            for nm, argv in refs.items():
                _, tr = _timed(argv + [smt2], _env(build), budget, run_once, startup[nm], "sat")
                if tr is not None:
                    arms[nm] = tr
            ours = max(ours, NOISE_S)       # compared with process times: same floor
    return st, ours, arms, fair


def collect(build: Path, jobs: int, n_api: int, budget: float,
            suites=("svcorpus", "rand-core", "sat-core", "fwhdl-core"), limit=None) -> dict:
    from tests.perf import suites as S, tools
    from tests.perf.run_sat import ref_arm, run_once
    from tests.perf.svcorpus import case as SC
    from dv_solve import stats
    refs = {}
    for nm in SMT2_REFS:
        p = tools.path(nm)
        if p is not None:
            refs[nm] = ref_arm(nm, p).argv
    startup = {"dv": _startup([str(build / "dv-solve-smt2")], _env(build), run_once, budget)}
    for nm, argv in refs.items():
        startup[nm] = _startup(argv, _env(build), run_once, budget)
    items = []
    if "svcorpus" in suites:
        items += [("sv", nm) for nm in SC.names()]
    if "rand-core" in suites:
        items += [("rand", b["name"]) for b in S.load("rand-core")["benches"]]
    for suite in ("sat-core", "fwhdl-core"):
        if suite in suites:
            items += [("smt2", str(_REPO / f["path"])) for f in S.load(suite)["fixtures"]]
    if limit:
        items = items[:limit]

    def one(item):
        kind, name = item
        try:
            if kind == "smt2":
                r = _smt2_item(name, build, refs, startup, budget, run_once)
                return item, (r + (False,)) if r else None
            return item, _rand_item(kind, name, build,
                                    {k: refs[k] for k in RAND_REFS if k in refs},
                                    startup, budget, run_once, n_api)
        except (subprocess.TimeoutExpired, OSError, ValueError) as e:
            return item, None

    with ThreadPoolExecutor(jobs) as ex:
        results = list(ex.map(one, items))
    frontier, per_item, files = {}, {}, []
    for (kind, name), r in results:
        if r is None:
            per_item[name] = None
            continue
        st, ours, arms, uniform = r
        best_single = min([ours] + list(arms.values()))
        best = min(ours, UNIFORM_ALLOWANCE * best_single) if uniform else best_single
        ratio = min(1.0, best / ours)
        files.append(st)
        for c in st["snapshot"]["classes"]:
            frontier[c["class"]] = min(ratio, frontier.get(c["class"], 1.0))
        per_item[name] = {"ours": ours, "arms": arms, "uniform": uniform, "ratio": ratio,
                          "classes": [c["class"] for c in st["snapshot"]["classes"]]}
    return {"schema": "dv-solve-perf-train/1", "stats": stats.merge(files),
            "frontier": frontier, "startup_s": startup, "items": per_item,
            "uniform_allowance": UNIFORM_ALLOWANCE}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build", help="build directory (lib/, dv-solve-smt2)")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--n-api", type=int, default=100, help="solves per API item")
    ap.add_argument("--budget", type=float, default=10.0, help="seconds per run")
    ap.add_argument("--suites", default="svcorpus,rand-core,sat-core,fwhdl-core")
    ap.add_argument("--limit", type=int, help="first N items only (a quick look)")
    a = ap.parse_args(argv)
    tr = collect(Path(a.build).resolve(), a.jobs, a.n_api, a.budget,
                 tuple(a.suites.split(",")), a.limit)
    Path(a.out).write_text(json.dumps(tr), encoding="utf-8")
    done = [v for v in tr["items"].values() if v]
    gaps = sorted(1 / v["ratio"] for v in done)
    print(f"{len(done)} of {len(tr['items'])} items; {len(tr['frontier'])} classes; "
          f"median gap to the best arm {gaps[len(gaps) // 2]:.2f}x, "
          f"{sum(g >= 4 for g in gaps)} items at 4x or more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
