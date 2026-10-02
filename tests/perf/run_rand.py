"""Randomization: draw N solutions per benchmark and arm, score cost and spread.

Arms (docs/results_publishing_design.md §2.1):

- `uniform`: a true RNG over the solution space, the floor for bias. Its
  scores at the same N are what a perfect sampler gets; better is noise.
- `<solver>-swizzle`: the solver behind Verilator 5.046's randomize()
  protocol (vlt_protocol.py), one process for all N calls, as in a
  simulation. `z3-swizzle` is what a Verilator user gets by default.
- `dv-swizzle-nohash`: dv-solve under the same protocol with
  `--verilator-hash=ignore`, which skips Verilator's parity asserts.
- `dv-api`: dv-solve's own API (the Python builder over the C library), one
  compiled context, reset and solved with a new seed each call: what SV DPI
  and zuspec users get.

Every model is checked against the benchmark's constraints with the IR's
evaluator; a model that violates them makes the run invalid.
"""
from __future__ import annotations

import math
import os
import random
import time
import zlib
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

from tests.formal.soundness.ir import ev, solutions
from tests.perf import vlt_protocol
from tests.perf.rand_benches import Bench
from tests.perf.run_sat import _pin_worker, physical_cores

_REPO = Path(__file__).resolve().parents[2]


@dataclass
class Arm:
    name: str
    kind: str                 # uniform | swizzle | api
    argv: list = field(default_factory=list)
    env: dict = field(default_factory=dict)
    build: str = "ref"        # head | anchor | ref


def make_arms(head: Path, anchor: Path | None, tools: dict) -> list:
    lib = {"DVS_SOLVER_PATH": str(Path(head).resolve().parent)}
    arms = [Arm("uniform", "uniform"),
            Arm("dv-api@head", "api", env=lib, build="head"),
            Arm("dv-swizzle@head", "swizzle",
                [str(head), "--interactive", "--mode=verilator"], build="head"),
            # The same build told to skip Verilator's parity asserts: both
            # settings are measured, so a benchmark where dv-solve's own spread
            # is worse than the parity constraints' shows on the page.
            Arm("dv-swizzle-nohash@head", "swizzle",
                [str(head), "--interactive", "--mode=verilator", "--verilator-hash=ignore"],
                build="head")]
    if anchor is not None:
        arms.append(Arm("dv-swizzle@anchor", "swizzle",
                        [str(anchor), "--interactive", "--mode=verilator"], build="anchor"))
    if "z3" in tools:
        arms.append(Arm("z3-swizzle", "swizzle", [tools["z3"], "-in"]))
    if "bitwuzla" in tools:
        arms.append(Arm("bitwuzla-swizzle", "swizzle", [tools["bitwuzla"]]))
    return arms


def _seed(bench: str, arm: str) -> int:
    return zlib.crc32(f"{bench}/{arm}".encode())


# ---- ground truth -----------------------------------------------------------

def truth(b: Bench) -> dict:
    """bin -> probability under a uniform draw from the solution space."""
    if b.bins is not None:
        return dict(b.bins[1])
    space = [tuple(m[n] for n in b.names) for m in solutions(b.widths, b.cons)]
    return {s: 1 / len(space) for s in space}


def _key(b: Bench, model: tuple):
    if b.bins is None:
        return model
    return b.bins[0](dict(zip(b.names, model)))


# ---- samplers ---------------------------------------------------------------

def _uniform(b: Bench, n: int, seed: int, space: list) -> dict:
    rng = random.Random(seed)
    if b.bins is None:
        models = [rng.choice(space) for _ in range(n)]
    else:                                   # rejection sampling over the bits
        models = []
        while len(models) < n:
            m = {k: rng.getrandbits(w) for k, w in b.widths.items()}
            if all(ev(c, m) for c in b.cons):
                models.append(tuple(m[k] for k in b.names))
    return {"models": models, "cpu_s": [], "wall_s": [], "checks": 0}


def _api(b: Bench, n: int, seed: int, env: dict) -> dict:
    # The head build's library and the checkout's Python package, not an
    # installed wheel (this runs in a pool worker, so the process is ours).
    os.environ.update(env)
    import sys
    if str(_REPO / "src") not in sys.path:
        sys.path.insert(0, str(_REPO / "src"))
    from dv_solve.builder import SolveProblemBuilder
    from dv_solve.ctx import SolveCtx, SOLVE_OK
    from tests.formal.soundness.doors import _lower
    ids = {k: i for i, k in enumerate(b.names)}
    bld = SolveProblemBuilder()
    for k in b.names:
        w = b.widths[k]
        bld.add_var(ids[k], width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
    for c in b.cons:
        bld.add_constraint(_lower(bld, c, ids))
    prob, _ = bld.finalize()
    rng = random.Random(seed)
    models, cpu, wall = [], [], []
    with SolveCtx(prob) as ctx:
        for _ in range(n):
            c0, w0 = time.process_time(), time.perf_counter()
            ctx.reset()
            rc = ctx.solve(seed=rng.getrandbits(63) + 1, fair_pick=True)
            if rc != SOLVE_OK:
                raise RuntimeError(f"dv-api: solve returned {rc}")
            m = tuple(ctx.get_value(ids[k]) & ((1 << b.widths[k]) - 1) for k in b.names)
            cpu.append(time.process_time() - c0)
            wall.append(time.perf_counter() - w0)
            models.append(m)
    return {"models": models, "cpu_s": cpu, "wall_s": wall, "checks": n}


def _swizzle(b: Bench, arm: Arm, n: int, seed: int) -> dict:
    r = vlt_protocol.sample(arm.argv, b.widths, b.smt_vlt(), n, seed, arm.env)
    return r


# ---- metrics ----------------------------------------------------------------

def _pct(xs: list, q: float) -> float:
    if not xs:
        return float("nan")
    s = sorted(xs)
    return s[min(len(s) - 1, int(q * len(s)))]


def score(b: Bench, models: list, tr: dict) -> dict:
    n = len(models)
    cnt = Counter(_key(b, m) for m in models)
    off = sum(v for k, v in cnt.items() if k not in tr)
    keys = list(tr)
    p = [cnt.get(k, 0) / n for k in keys]
    q = [tr[k] for k in keys]

    def kl(a, c):
        return sum(ai * math.log2(ai / ci) for ai, ci in zip(a, c) if ai > 0)

    mid = [(a + c) / 2 for a, c in zip(p, q)]
    jsd = 0.5 * kl(p, mid) + 0.5 * kl(q, mid)
    chi2 = sum((cnt.get(k, 0) - n * tr[k]) ** 2 / (n * tr[k]) for k in keys)
    try:
        from scipy.stats import chi2 as _c2
        pval = float(_c2.sf(chi2, len(keys) - 1))
    except ImportError:
        pval = None
    out = {"n": n, "cov": len([k for k in keys if cnt.get(k)]) / len(keys),
           "jsd": jsd, "chi2p": pval, "distinct": len(set(models)) / n, "off": off}
    if b.thin is not None:
        tset = [k for k in keys if b.thin(dict(zip(b.names, k)))]
        share = sum(tr[k] for k in tset)
        got = sum(cnt.get(k, 0) for k in tset) / n
        out["thin"] = got / share if share else None
    # sorted bin frequencies, normalised to the uniform expectation, for the
    # histogram small multiples (at most 64 points, evenly spaced ranks)
    freq = sorted((cnt.get(k, 0) / (n * tr[k]) for k in keys), reverse=True)
    step = max(1, len(freq) // 64)
    out["hist"] = [round(f, 3) for f in freq[::step]]
    return out


def bad_models(b: Bench, models: list) -> int:
    """Models that violate the constraints: a wrong answer."""
    seen, bad = set(), 0
    for m in models:
        if m in seen:
            continue
        seen.add(m)
        if not all(ev(c, dict(zip(b.names, m))) for c in b.cons):
            bad += 1
    return bad


def run_one(job: tuple) -> dict:
    bname, arm, n, space = job
    b = Bench(bname)
    seed = _seed(bname, arm.name)
    try:
        if arm.kind == "uniform":
            r = _uniform(b, n, seed, space)
        elif arm.kind == "api":
            r = _api(b, n, seed, arm.env)
        else:
            r = _swizzle(b, arm, n, seed)
    except Exception as e:                  # recorded, never fatal to the suite
        return {"bench": bname, "arm": arm.name, "build": arm.build, "error": str(e)[:200]}
    tr = truth(b) if space is None else {s: 1 / len(space) for s in space}
    row = {"bench": bname, "arm": arm.name, "build": arm.build,
           **score(b, r["models"], tr), "bad": bad_models(b, r["models"])}
    if arm.kind != "uniform":
        cpu = r["cpu_s"]
        row["cpu_ms"] = (sum(cpu) if isinstance(cpu, list) else cpu) * 1000 / n
        row["p50_ms"] = _pct(r["wall_s"], 0.50) * 1000
        row["p95_ms"] = _pct(r["wall_s"], 0.95) * 1000
        row["p99_ms"] = _pct(r["wall_s"], 0.99) * 1000
        row["checks"] = r["checks"] / n
    return row


def check(rows: list) -> str:
    """Why the run is invalid ('' if it is not): a model from the head build
    that violates its constraints. Other arms' bad models are recorded only."""
    bad = [f"{r['bench']}/{r['arm']}: {r['bad']} bad models" for r in rows
           if r.get("bad") and r["build"] == "head"]
    return "; ".join(bad)


def run_suite(spec: dict, arms: list, workers: int = 0) -> dict:
    benches = spec["benches"]
    spaces = {}
    for e in benches:
        b = Bench(e["name"])
        spaces[e["name"]] = (None if b.bins is not None else
                             [tuple(m[k] for k in b.names) for m in solutions(b.widths, b.cons)])
    jobs = [(e["name"], a, e.get("n", spec["n"]), spaces[e["name"]])
            for e in benches for a in arms]
    cores = physical_cores()
    workers = workers or max(1, len(cores) // 4)
    import multiprocessing as mp
    q = mp.get_context("fork").Queue()
    for c in cores[:workers]:
        q.put(c)
    with ProcessPoolExecutor(workers, mp_context=mp.get_context("fork"),
                             initializer=_pin_worker, initargs=(q,)) as ex:
        rows = list(ex.map(run_one, jobs))
    return {"rows": rows, "invalid": check(rows)}
