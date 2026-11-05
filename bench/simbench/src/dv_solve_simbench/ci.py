"""Nightly simbench in CI: one run record per night (docs/simbench_nightly_plan.md §4).

    python3 -m dv_solve_simbench.ci --kind nightly --out sim-out [--github-output]

The README is the procedure, and this runs it as written. It is not a
different procedure: the same pytest commands, one config at a time, serially,
behind the same checks. Then it wraps the per-(config, bench) records into one
run record:

1. dv-solve: `--build-info` of dv-solve-smt2 and bench_vlt_replay must say
   optimized=1 asan=0, and the library loaded must be the build's own.
2. Pre-flight (README §5): bench_vlt_replay --calib and the smoke tier of
   vlt-dvs-solver and dvs-standalone, against the README's reference.
3. Each variant: `pytest -k "<config>-" --simbench-out=...`, with the dumps
   kept (random spread) and the solver sessions kept.
4. The run record: `<out>/<artifact>.json.gz`, with every result keyed by
   variant, simulator build and dv-solve build. The full JSONL, dumps and
   sessions stay in <out>/raw for a second, diagnostic artifact.

Before the pre-flight and before each variant it waits for a quiet host
(wait_quiet): the runner shares the machine with push-triggered CI, and the
concurrency group keeps out only perf.yml. A dispatch that landed beside a
test.yml run measured calib at twice its reference.

A failed check marks the record invalid. It is still written, and the exit
status is 1, so the workflow uploads it and then goes red. Exit 1 also follows
a wrong answer (a check error) from a dv-solve variant: that is our bug. A
wrong answer from another simulator (xezim, or Verilator with z3) is a result
about that simulator, so it is recorded and shown, and the run stays valid.
A bench a simulator cannot take (xezim does not elaborate it) is
`unsupported`. It counts as coverage, not as a failure.

Tools come from the environment, as for a person: SIMBENCH_VERILATOR_SC,
SIMBENCH_XEZIM, SIMBENCH_DVS_BUILD (or SIMBENCH_DVS_SMT2/_REPLAY), z3 on PATH.
In CI both simulators float (the newest release, `tests.perf.tools latest`;
plan §2), and `--sims` records how each was resolved. The pre-flight checks
the dv-solve build against README §5, which was measured on the pinned
Verilator, so SIMBENCH_PREFLIGHT_VERILATOR_SC (the lock's pin) runs it when
set: a new Verilator that legitimately moves a number must not invalidate the
run that shows it.

Each dvs-standalone bench row carries `ses`, the hash of the solver session
Verilator sent. A step in the series with new hashes is Verilator's (it sent
a different problem); one with the same hashes is dv-solve's.
"""
import argparse
import gzip
import hashlib
import json
import os
import subprocess
import sys
import time
from typing import Dict, List, Optional

from . import provenance, records

# Published variant id -> simbench config id. None: reserved, not runnable yet.
VARIANTS: Dict[str, Optional[str]] = {
    "vlt-z3-default": "vlt-z3-default",
    "vlt-dvs-smt": "vlt-dvs-solver",
    "dvs-standalone": "dvs-standalone",
    "xezim": "xezim",
    "vlt-dvs-lib": None,
}
RESERVED = {"vlt-dvs-lib": "no simulator support (Verilator cannot load libdv_solve yet)"}
# Variants whose numbers depend on the dv-solve build.
DVS_VARIANTS = ("vlt-dvs-smt", "dvs-standalone", "vlt-dvs-lib")
_NIGHTLY = ["vlt-dvs-smt", "dvs-standalone", "xezim", "vlt-dvs-lib"]
# The reference runs first, so the others' chi2 has its dumps (SIMBENCH_REF_CONFIG).
KINDS = {
    "nightly": _NIGHTLY,
    "manual": _NIGHTLY,
    "reference": ["vlt-z3-default"] + _NIGHTLY,
    "release": ["vlt-dvs-smt", "dvs-standalone"],
}

# README §5 reference: AMD Ryzen 9 9950X, Release, GCC 13.3, idle, 2026-11-04,
# dv-solve 17fb20a, Verilator 5.054, N=200, seed 1. Keep the two in step.
CALIB_REF = {"cpu_ns_per_iter": 1.05, "dvs_solve_us": 0.72, "dvs_compile_us": 3.8}
CALIB_RATIO_TOL = 1.5          # dvs_solve_us / cpu_ns_per_iter vs 0.72 / 1.05
# (config, bench): (us/call, min entropy of the answers timed)
PREFLIGHT_REF = {
    ("dvs-standalone", "construct.range"): (7.3, 0.809),
    ("dvs-standalone", "dist.weighted"): (27, 0.179),
    ("dvs-standalone", "memmap.soc_mem_map"): (502, 0.258),
    ("dvs-standalone", "scenario.axi4_burst"): (8.1, 0.059),
    ("dvs-standalone", "scenario.bus_transaction"): (63, 0.259),
    ("dvs-standalone", "scenario.reset_reuse"): (4.1, 0.669),
    ("vlt-dvs-solver", "construct.range"): (69, 0.817),
    ("vlt-dvs-solver", "dist.weighted"): (52, 0.201),
    ("vlt-dvs-solver", "memmap.soc_mem_map"): (7300, 0.271),
    ("vlt-dvs-solver", "scenario.axi4_burst"): (47, 0.059),
    ("vlt-dvs-solver", "scenario.bus_transaction"): (1250, 0.261),
    ("vlt-dvs-solver", "scenario.reset_reuse"): (27, 0.670),
}
# Seed 1 repeats exactly; the margin is for a deliberate, small sampler change.
PREFLIGHT_ENTROPY_TOL = 0.05
PREFLIGHT_TOL = 3.0
PREFLIGHT_TESTS = 12

_HERE = os.path.dirname(os.path.abspath(__file__))
_SIMBENCH = os.path.dirname(os.path.dirname(_HERE))     # bench/simbench


class Check(Exception):
    """A failed check: the numbers would be wrong. The record is invalid."""


def _log(msg: str) -> None:
    print("simbench-ci: " + msg, flush=True)


# --- checks -----------------------------------------------------------------

def check_build(info: Optional[dict], what: str) -> dict:
    """README §3.1: an optimised, non-ASAN dv-solve build, loading its own library."""
    if not info or info.get("error") or "build_info" not in info:
        raise Check("%s: no --build-info (%s)" % (what, (info or {}).get("error", "not found")))
    if info.get("optimized") != 1 or info.get("asan") != 0:
        raise Check("%s: build is not Release (optimized=%s asan=%s)"
                    % (what, info.get("optimized"), info.get("asan")))
    # dv-solve-smt2 links the library statically (it names itself); a
    # dynamically linked binary must load <its build>/lib/libdv_solve.so.
    lib, binary = info.get("library"), info.get("binary") or ""
    static = lib and os.path.realpath(lib) == os.path.realpath(binary)
    if lib and not static and os.path.dirname(os.path.dirname(lib)) != os.path.dirname(binary):
        raise Check("%s: loads %s, not its own build's library (LD_LIBRARY_PATH?)" % (what, lib))
    return info


def check_calib(calib: dict) -> None:
    """README §5: the dv-solve build's speed relative to the host's."""
    cpu, dvs = calib.get("cpu_ns_per_iter"), calib.get("dvs_solve_us")
    if not cpu or not dvs:
        raise Check("bench_vlt_replay --calib gave no reading: %r" % calib)
    ratio, ref = dvs / cpu, CALIB_REF["dvs_solve_us"] / CALIB_REF["cpu_ns_per_iter"]
    if ratio > ref * CALIB_RATIO_TOL:
        raise Check("calib: dvs_solve_us/cpu_ns_per_iter %.2f, reference %.2f (x%.1f allowed): "
                    "the dv-solve build is slow" % (ratio, ref, CALIB_RATIO_TOL))


def check_preflight(recs: List[dict], calib: dict) -> dict:
    """README §5 pass criteria over the smoke-tier records."""
    if len(recs) != PREFLIGHT_TESTS:
        raise Check("pre-flight: %d records, want %d" % (len(recs), PREFLIGHT_TESTS))
    bad = [(r["simbench"]["config"], r["simbench"]["bench"], r.get("reason")) for r in recs
           if not r["valid"]]
    if bad:
        raise Check("pre-flight: failed %s" % bad)
    scale = calib["dvs_solve_us"] / CALIB_REF["dvs_solve_us"]
    out = {}
    for r in recs:
        key = (r["simbench"]["config"], r["simbench"]["bench"])
        name = "%s/%s" % key
        us = r["simbench"]["result"]["ns_per_call"] / 1e3
        spread = _spread(r) or {}
        out[name] = {"us": round(us, 3), "min_entropy": spread.get("min_entropy")}
        if key not in PREFLIGHT_REF:
            continue
        ref_us, ref_ent = PREFLIGHT_REF[key]
        if us / scale > ref_us * PREFLIGHT_TOL:
            raise Check("pre-flight: %s %.1f us/call (%.1f scaled), reference %g (x%g allowed)"
                        % (name, us, us / scale, ref_us, PREFLIGHT_TOL))
        # Speed alone let a sampler that never moved its fields set the
        # 2026-10-28 reference; the spread of what was timed is checked too.
        if spread.get("stuck") or spread.get("invalid"):
            raise Check("pre-flight: %s has %s stuck field(s) and %s invalid model(s)"
                        % (name, spread.get("stuck"), spread.get("invalid")))
        ent = spread.get("min_entropy")
        if ent is None or ent < ref_ent - PREFLIGHT_ENTROPY_TOL:
            raise Check("pre-flight: %s min entropy %s, reference %.3f (-%.2f allowed)"
                        % (name, ent, ref_ent, PREFLIGHT_ENTROPY_TOL))
    return out


# --- a quiet host -----------------------------------------------------------

QUIET_LOAD = 2.0         # 1-minute load average below this ...
QUIET_SETTLE_S = 60      # ... for this long
QUIET_TIMEOUT_S = 1800   # then give up and measure anyway (the record says so)
QUIET_POLL_S = 15


def wait_quiet(max_load: float = QUIET_LOAD, settle_s: float = QUIET_SETTLE_S,
               timeout_s: float = QUIET_TIMEOUT_S, poll_s: float = QUIET_POLL_S,
               read=None, sleep=time.sleep, now=time.monotonic) -> dict:
    """Wait until the load average is under `max_load`; once it has been over,
    it must then stay under for `settle_s`.

    Returns what happened, for the record: {"quiet", "waited_s", "load_start",
    "load_end"}. quiet=False means `timeout_s` passed first; the run goes on and
    machine.noisy marks it as usual. timeout_s=0 does not wait.
    """
    read = read or records._loadavg
    t0 = now()
    load0 = load = read()
    if load < max_load:          # the 1-minute average already covers the last minute
        return {"quiet": True, "waited_s": 0.0, "load_start": load0, "load_end": load}
    calm_since = None
    while timeout_s > 0:
        t = now()
        if calm_since is not None and t - calm_since >= settle_s:
            break
        if t - t0 >= timeout_s:
            return {"quiet": False, "waited_s": round(t - t0, 1), "load_start": load0,
                    "load_end": load}
        sleep(poll_s)
        load = read()
        if load >= max_load:
            calm_since = None
        elif calm_since is None:
            calm_since = now()
    return {"quiet": True, "waited_s": round(now() - t0, 1), "load_start": load0,
            "load_end": load}


# --- per-bench outcome ------------------------------------------------------

def outcome(rec: dict) -> str:
    """ok | unsupported | failed | wrong | error.

    wrong: a value broke the constraints (check_errors); it invalidates the
    run for a dv-solve variant only (module docstring). unsupported: xezim could not elaborate the bench (the build step is
    its elaboration). failed: ran, but randomize() failed or timed out.
    """
    if rec["valid"]:
        return "ok"
    res = rec["simbench"].get("result") or {}
    if res.get("check_errors"):
        return "wrong"
    reason = rec.get("reason") or ""
    if rec["tools"].get("simulator") == "xezim" and reason.startswith("BuildError"):
        return "unsupported"
    if res:
        return "failed"
    return "error"


def _spread(rec: dict) -> Optional[dict]:
    """Random spread of the answers this variant timed (plan §6)."""
    rand = (rec["simbench"].get("result") or {}).get("randomization")
    if not rand:
        return None
    if rand.get("source") == "capture":            # replay: the replay's own answers
        q = rand.get("replay")
        if not q:
            return None
        return {k: q.get(k) for k in ("models", "invalid", "n_fields", "forced", "stuck",
                                       "min_distinct", "min_entropy")}
    chi2 = [f["chi2"] for f in rand.get("fields", []) if f.get("chi2") is not None]
    return {"n": rand.get("n"), "min_distinct": rand.get("min_distinct"),
            "min_entropy": rand.get("min_entropy"), "chi2_max": max(chi2) if chi2 else None,
            "fields": [[f.get("distinct"), f.get("entropy")] for f in rand.get("fields", [])]}


def bench_row(rec: dict) -> dict:
    res = rec["simbench"].get("result") or {}
    row = {"bench": rec["simbench"]["bench"], "outcome": outcome(rec),
           "us": round(res["ns_per_call"] / 1e3, 4) if res.get("ns_per_call") else None,
           "first_us": round(res["first_call_ns"] / 1e3, 3) if res.get("first_call_ns") else None,
           "failures": res.get("failures"), "check_errors": res.get("check_errors"),
           "spread": _spread(rec)}
    ses = (rec["simbench"].get("session") or {}).get("sha256")
    if ses:
        row["ses"] = ses[:12]
    if not rec["valid"]:
        row["reason"] = (rec.get("reason") or "")[:300]
    return row


def manifest_benches(recs: List[dict]) -> List[list]:
    """[[bench, bench.sv sha256 (12)]] in manifest order: the order of the
    trend line's per-bench arrays."""
    return [[b, (sha or "")[:12]] for b, sha in sorted(
        {(r["simbench"]["bench"], r["simbench"]["bench_sha"] or "") for r in recs})]


def manifest_hash(recs: List[dict]) -> str:
    """The bench set and sources: same hash, same benches (plan §6)."""
    h = hashlib.sha256()
    for b, sha in sorted({(r["simbench"]["bench"], r["simbench"]["bench_sha"] or "")
                          for r in recs}):
        h.update(("%s %s\n" % (b, sha)).encode())
    return h.hexdigest()[:12]


def sim_identity(build: Optional[dict]) -> Optional[str]:
    """Series label: verilator@5.054 / xezim@0.11.3+058f2bb.48820a8 (plan §2)."""
    if not build:
        return None
    if build.get("simulator") == "xezim":
        core = ((build.get("build_info") or {}).get("xezim-core") or {}).get("sha", "")
        return "xezim@%s+%s.%s" % (build.get("version"), (build.get("git") or "")[:7], core[:7])
    rel = (build.get("release") or {}).get("version")
    ver = rel or (build.get("version_text") or "").split()[1:2]
    return "%s@%s" % (build.get("simulator"), ver if isinstance(ver, str) else "".join(ver))


def dvs_identity(build: Optional[dict]) -> Optional[dict]:
    if not build or "build_info" not in build:
        return None
    return {k: build.get(k) for k in ("version", "commit", "dirty", "build_type", "compiler")
            if build.get(k) is not None}


def variant_block(variant: str, config: str, recs: List[dict]) -> dict:
    counts: Dict[str, int] = {}
    rows = [bench_row(r) for r in recs]
    for row in rows:
        counts[row["outcome"]] = counts.get(row["outcome"], 0) + 1
    tools = recs[0]["tools"] if recs else {}
    build = tools.get("solver_build") or tools.get("replay_build")
    block = {"config": config, "sim": sim_identity(tools.get("simulator_build")),
             "simulator_build": tools.get("simulator_build"),
             "dvs": dvs_identity(build) if variant in DVS_VARIANTS else None,
             "counts": counts, "benches": rows}
    us = [r["us"] for r in rows if r["outcome"] == "ok" and r["us"]]
    if us:
        import math
        block["geo_us"] = round(math.exp(sum(math.log(u) for u in us) / len(us)), 4)
    return block


# --- running ----------------------------------------------------------------

def _pytest(k: str, out_jsonl: str, log: str, env: dict, marker: Optional[str] = None) -> int:
    cmd = [sys.executable, "-m", "pytest", "-q", "-rs", "-p", "no:cacheprovider",
           "-k", k, "tests/test_bench.py", "--simbench-out=" + out_jsonl]
    if marker:
        cmd[3:3] = ["-m", marker]
    with open(log, "w") as fp:
        fp.write("$ %s\n" % " ".join(cmd))
        fp.flush()
        p = subprocess.run(cmd, cwd=_SIMBENCH, env=env, stdout=fp, stderr=subprocess.STDOUT)
    return p.returncode


def _env(raw: str) -> dict:
    env = dict(os.environ)
    env.setdefault("PYTHONPATH", os.path.join(_SIMBENCH, "src"))
    env.update(SIMBENCH_BUILD_DIR=os.path.join(raw, "build"),
               SIMBENCH_SESSION_DIR=os.path.join(raw, "sessions"),
               SIMBENCH_DUMP_DIR=os.path.join(raw, "dumps"),
               SIMBENCH_REF_CONFIG="vlt-z3-default")
    # The machine block names the machine class only: no host label in CI.
    env.pop("SIMBENCH_HOST", None)
    return env


def _git_describe() -> Optional[str]:
    try:
        p = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=_SIMBENCH,
                           capture_output=True, text=True)
        return p.stdout.strip() or None
    except OSError:
        return None


def collect(kind: str, variants: List[str], out: str, n: int, seed: int,
            marker: Optional[str], release: Optional[str], lock_sha: Optional[str],
            quiet: Optional[dict] = None, sims: Optional[dict] = None) -> dict:
    from .config import dvs_defaults, load_configs
    raw = os.path.join(out, "raw")
    os.makedirs(raw, exist_ok=True)
    env = _env(raw)
    env.update(SIMBENCH_N=str(n), SIMBENCH_SEED=str(seed))
    utc = records.utc_now()
    load0 = records._loadavg()
    run = dict(records.run_info(), utc=utc, dv_version=_git_describe(), release=release,
               workflow_run=os.environ.get("GITHUB_RUN_NUMBER"))
    if run["commit"] == "unknown" and os.environ.get("GITHUB_SHA"):
        run["commit"] = os.environ["GITHUB_SHA"]      # a checkout without .git
    rec = {"schema": 1, "valid": True, "kind": kind, "family": "sim", "run": run,
           "machine": None, "calib": {},
           "tools": {"lock_sha": lock_sha,
                     # how each simulator was resolved: version, url, sha256, src (latest|lock)
                     "sims": {k: {f: v.get(f) for f in ("version", "url", "sha256", "src", "error")
                                  if v.get(f) is not None}
                              for k, v in (sims or {}).items()}},
           "sim": {"n": n, "seed": seed, "tier": marker or "all", "preflight": None,
                   "variants": {}, "quiet_waits": {}}}
    quiet = dict(quiet or {})

    def settle(what: str) -> None:
        w = wait_quiet(**quiet)
        rec["sim"]["quiet_waits"][what] = w
        if w["waited_s"] >= QUIET_POLL_S or not w["quiet"]:
            _log("%s: waited %.0f s for a quiet host (load %.1f -> %.1f)%s"
                 % (what, w["waited_s"], w["load_start"], w["load_end"],
                    "" if w["quiet"] else "; gave up, measuring anyway"))
    try:
        if any(v in DVS_VARIANTS and VARIANTS[v] for v in variants):
            dvs = {k: os.environ.get(k) or v for k, v in dvs_defaults().items()}
            rec["tools"]["dvs_smt2"] = check_build(
                provenance.build_info(dvs["SIMBENCH_DVS_SMT2"]), "dv-solve-smt2")
            rec["tools"]["dvs_replay"] = check_build(
                provenance.build_info(dvs["SIMBENCH_DVS_REPLAY"]), "bench_vlt_replay")
            settle("preflight")
            calib = provenance.calib(dvs["SIMBENCH_DVS_REPLAY"])
            rec["calib"] = calib
            check_calib(calib)
            _log("pre-flight")
            pre = os.path.join(raw, "preflight.jsonl")
            pre_env = dict(env)
            if env.get("SIMBENCH_PREFLIGHT_VERILATOR_SC"):
                pre_env["SIMBENCH_VERILATOR_SC"] = env["SIMBENCH_PREFLIGHT_VERILATOR_SC"]
            rec["sim"]["preflight_sim"] = sim_identity(provenance.simulator_build(
                "verilator", pre_env.get("SIMBENCH_VERILATOR_SC") or "verilator", "--version"))
            _pytest("dvs-standalone- or vlt-dvs-solver-", pre, os.path.join(raw, "preflight.log"),
                    pre_env, marker="smoke")
            rec["sim"]["preflight"] = check_preflight(records.load(pre), calib)
        cfgs = load_configs(environ=env)
        results = os.path.join(raw, "results.jsonl")
        for v in variants:
            config = VARIANTS[v]
            if config is None:
                rec["sim"]["variants"][v] = {"skipped": RESERVED[v]}
                continue
            cfg = cfgs[config]
            if not cfg.available:
                raise Check("%s (%s): %s" % (v, config, cfg.reason))
            settle(v)
            _log("%s (%s)" % (v, config))
            before = len(records.load(results)) if os.path.exists(results) else 0
            t0 = time.time()
            _pytest(config + "-", results, os.path.join(raw, v + ".log"), env, marker)
            recs = [r for r in records.load(results)[before:] if r["simbench"]["config"] == config]
            if not recs:
                raise Check("%s: no records (see raw/%s.log)" % (v, v))
            block = variant_block(v, config, recs)
            block["wall_s"] = round(time.time() - t0, 1)
            rec["sim"]["variants"][v] = block
            _log("%s: %s" % (v, block["counts"]))
        allrecs = records.load(results) if os.path.exists(results) else []
        rec["sim"]["manifest"] = manifest_hash(allrecs)
        rec["sim"]["benches"] = manifest_benches(allrecs)
        wrong = [(v, b["bench"]) for v, blk in rec["sim"]["variants"].items()
                 if v in DVS_VARIANTS
                 for b in blk.get("benches", []) if b["outcome"] == "wrong"]
        if wrong:
            raise Check("wrong answers (check errors): %s" % wrong)
    except Check as e:
        rec["valid"], rec["reason"] = False, str(e)
        _log("INVALID: %s" % e)
    rec["sim"]["noisy"] = any(not w["quiet"] for w in rec["sim"]["quiet_waits"].values())
    rec["machine"] = {k: v for k, v in records.machine(load0, records._loadavg()).items()
                      if k != "label"}
    return rec


def artifact_name(kind: str, utc: str, commit: str) -> str:
    return "simbench-%s-%s-%s" % (kind, utc, commit[:7])


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--kind", choices=sorted(KINDS), default="manual")
    ap.add_argument("--variants", default=None,
                    help="comma-separated variant ids (default: the kind's set): "
                    + ", ".join(VARIANTS))
    ap.add_argument("--out", default="sim-out")
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("-m", "--marker", default=None, help="bench tier (smoke/standard/stress); default all")
    ap.add_argument("--release", default=None, help="dv-solve release tag the build is (kind=release)")
    ap.add_argument("--lock-sha", default=None, help="tests.perf.tools.lock_sha('sim')")
    ap.add_argument("--quiet-load", type=float, default=QUIET_LOAD,
                    help="wait for the 1-minute load average to stay under this (default %(default)s)")
    ap.add_argument("--quiet-timeout", type=float, default=QUIET_TIMEOUT_S,
                    help="seconds to wait for a quiet host before measuring anyway; 0: no wait")
    ap.add_argument("--sims", default=None,
                    help="JSON from `tests.perf.tools latest`: how each simulator was resolved")
    ap.add_argument("--github-output", action="store_true")
    a = ap.parse_args(argv)
    variants = a.variants.split(",") if a.variants else KINDS[a.kind]
    unknown = [v for v in variants if v not in VARIANTS]
    if unknown:
        ap.error("unknown variant(s) %s" % unknown)
    out = os.path.abspath(a.out)
    rec = collect(a.kind, variants, out, a.n, a.seed, a.marker, a.release, a.lock_sha,
                  quiet={"max_load": a.quiet_load, "timeout_s": a.quiet_timeout},
                  sims=json.load(open(a.sims)) if a.sims else None)
    name = artifact_name(a.kind, rec["run"]["utc"], rec["run"]["commit"])
    path = os.path.join(out, name + ".json.gz")
    with gzip.open(path, "wt") as fp:
        json.dump(rec, fp, sort_keys=True)
    _log("wrote %s (valid=%s)" % (path, rec["valid"]))
    if a.github_output and os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as fp:
            fp.write("name=%s\nraw=%s\n" % (name, name.replace("simbench-", "simbench-raw-", 1)))
    return 0 if rec["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
