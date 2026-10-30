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
- `<solver>-seed`: one process per draw, a plain SMT-LIB2 script that sets
  `(set-option :random-seed k)`: what fw-hdl does for stimulus
  (docs/fw_hdl_implementation_plan.md piece 6.4).

Every model is checked against the benchmark's constraints with the IR's
evaluator; a model that violates them makes the run invalid.
"""
from __future__ import annotations

import json
import math
import os
import random
import resource
import subprocess
import time
import zlib
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

from tests.formal.soundness.ir import ev, smt, solutions
from tests.perf import vlt_protocol
from tests.perf.rand_benches import Bench
from tests.perf.run_sat import _pin_worker, physical_cores

_REPO = Path(__file__).resolve().parents[2]


@dataclass
class Arm:
    name: str
    kind: str                 # uniform | swizzle | api | capi | seed
    argv: list = field(default_factory=list)
    env: dict = field(default_factory=dict)
    build: str = "ref"        # head | anchor | ref


def make_arms(head: Path, anchor: Path | None, tools: dict, api: bool = True) -> list:
    """api=False leaves out the arms that load the head build's library from
    this checkout's Python/C bench code: a release build's library predates
    that code (missing symbols, no bench_rand_capi), so a release run has
    only the arms that drive its dv-solve-smt2."""
    lib = {"DVS_SOLVER_PATH": str(Path(head).resolve().parent)}
    arms = [Arm("uniform", "uniform")]
    if api:
        arms += [Arm("dv-api@head", "api", env=lib, build="head"),
                 Arm("dv-capi@head", "capi", [str(Path(head).resolve().parent / "bench_rand_capi")],
                     env=lib, build="head")]
    arms += [Arm("dv-swizzle@head", "swizzle",
                 [str(head), "--interactive", "--mode=verilator"], build="head"),
             # The same build told to skip Verilator's parity asserts: both
             # settings are measured, so a benchmark where dv-solve's own spread
             # is worse than the parity constraints' shows on the page.
             Arm("dv-swizzle-nohash@head", "swizzle",
                 [str(head), "--interactive", "--mode=verilator", "--verilator-hash=ignore"],
                 build="head"),
             Arm("dv-seed@head", "seed", [str(head)], build="head")]
    if anchor is not None:
        arms.append(Arm("dv-swizzle@anchor", "swizzle",
                        [str(anchor), "--interactive", "--mode=verilator"], build="anchor"))
    if "z3" in tools:
        arms.append(Arm("z3-swizzle", "swizzle", [tools["z3"], "-in"]))
        arms.append(Arm("z3-seed", "seed", [tools["z3"], "-in"]))
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
    elif len(b.bins) > 2:                   # the bench's own exact draw
        models = [tuple(m[k] for k in b.names) for m in (b.bins[2](rng) for _ in range(n))]
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


def _problem_bytes(b: Bench) -> bytes:
    """The bench as a problem buffer, lowered as the API arm lowers it."""
    import sys
    if str(_REPO / "src") not in sys.path:
        sys.path.insert(0, str(_REPO / "src"))
    from dv_solve.builder import SolveProblemBuilder
    from tests.formal.soundness.doors import _lower
    ids = {k: i for i, k in enumerate(b.names)}
    bld = SolveProblemBuilder()
    for k in b.names:
        w = b.widths[k]
        bld.add_var(ids[k], width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
    for c in b.cons:
        bld.add_constraint(_lower(bld, c, ids))
    return bld.finalize_bytes()


def _capi(b: Bench, arm: Arm, n: int, seed: int) -> dict:
    """tests/c/bench_rand_capi: the C API with no Python in the timed path.
    Times are the binary's per-draw thread CPU time, so cpu_s and wall_s are
    the same list."""
    import tempfile
    os.environ.update(arm.env)      # the builder loads this build's library too
    with tempfile.TemporaryDirectory() as d:
        pf, mf = Path(d) / "p.dvsp", Path(d) / "m.out"
        pf.write_bytes(_problem_bytes(b))
        p = subprocess.run([*arm.argv, str(pf), str(len(b.names)), str(n), str(seed), str(mf)],
                           capture_output=True, text=True, env={**os.environ, **arm.env})
        summ = json.loads(p.stdout.strip().splitlines()[-1])
        if summ.get("compile_rc") or summ.get("ok") != n:
            raise RuntimeError(f"{arm.name}: {summ}")
        masks = [(1 << b.widths[k]) - 1 for k in b.names]
        models, ts = [], []
        for line in mf.read_text().splitlines():
            f = line.split()
            ts.append(float(f[1]) * 1e-9)
            models.append(tuple(int(v) & mk for v, mk in zip(f[2:], masks)))
    return {"models": models, "cpu_s": ts, "wall_s": ts, "checks": n}


def _swizzle(b: Bench, arm: Arm, n: int, seed: int) -> dict:
    r = vlt_protocol.sample(arm.argv, b.widths, b.smt_vlt(), n, seed, arm.env)
    return r


def seed_script(b: Bench, k: int) -> str:
    """One draw: the benchmark as a QF_BV script under `:random-seed k`."""
    return ("(set-option :produce-models true)\n"
            f"(set-option :random-seed {k})\n(set-logic QF_BV)\n"
            + "".join(f"(declare-fun {n} () (_ BitVec {b.widths[n]}))\n" for n in b.names)
            + "".join(f"(assert {smt(c)})\n" for c in b.cons)
            + "(check-sat)\n(get-value (" + " ".join(b.names) + "))\n")


def _seeded(b: Bench, arm: Arm, n: int, seed: int) -> dict:
    rng = random.Random(seed)
    env = {**os.environ, **arm.env}
    models, cpu, wall = [], [], []
    for _ in range(n):
        ru0 = resource.getrusage(resource.RUSAGE_CHILDREN)
        w0 = time.perf_counter()
        p = subprocess.run(arm.argv, input=seed_script(b, rng.getrandbits(31) + 1),
                           capture_output=True, text=True, env=env,
                           timeout=vlt_protocol.CALL_BUDGET_S, preexec_fn=vlt_protocol._limits)
        wall.append(time.perf_counter() - w0)
        ru1 = resource.getrusage(resource.RUSAGE_CHILDREN)
        cpu.append(ru1.ru_utime - ru0.ru_utime + ru1.ru_stime - ru0.ru_stime)
        if not p.stdout.startswith("sat"):
            raise RuntimeError(f"{arm.name}: {p.stdout[:80]!r}")
        m = {k: vlt_protocol._value(lit, dec) for k, lit, dec in vlt_protocol._VAL.findall(p.stdout)}
        models.append(tuple(m[k] for k in b.names))
    return {"models": models, "cpu_s": cpu, "wall_s": wall, "checks": n}


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
        elif arm.kind == "seed":
            r = _seeded(b, arm, n, seed)
        elif arm.kind == "capi":
            r = _capi(b, arm, n, seed)
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
