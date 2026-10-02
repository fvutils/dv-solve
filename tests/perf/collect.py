"""Run the perf suites and write one gzipped run record (design §6.1).

    python3 -m tests.perf.collect --kind manual --suites calib,sat-core,rand-core --out perf-out

The head build is build/dv-solve-smt2 (--head-bin to override); the anchor
release is built from its tag under --build-root. Pinned solvers come from
$DVS_PERF_TOOLS or a developer checkout's packages/ (tests/perf/tools.py).

Writes perf-out/<artifact-name>.json.gz and prints the artifact name. With
--github-output, also appends `name=<artifact-name>` to $GITHUB_OUTPUT so the
workflow can name the artifact after the record.

An invalid record (a wrong answer anywhere) is still written, with
`valid: false` and a reason, and the exit status is 1 so the job fails after
the record has been uploaded.
"""
from __future__ import annotations

import argparse
import datetime
import gzip
import json
import os
import subprocess
import sys
from pathlib import Path

from . import builds, calib, run_rand, run_sat, schema, suites, tools

SUITES = ("calib", "sat-core", "rand-core")
NOISY_LOAD = 4.0             # host 1-min load above this marks the run noisy


def _commit() -> str:
    sha = os.environ.get("GITHUB_SHA")
    if sha:
        return sha
    return subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                          text=True, check=True).stdout.strip()


def _dv_version():
    try:
        from importlib.metadata import version
        return version("dv-solve")
    except Exception:
        return None


def _sat(rec: dict, name: str, a) -> None:
    spec = suites.load(name)
    head = builds.head_bin(a.head_bin)
    anchor = _anchor(rec, a)
    paths = {n: tools.path(n) for n in ("z3", "bitwuzla", "boolector")}
    arms = run_sat.make_arms(head, anchor, paths)
    res = run_sat.run_suite(spec, arms, a.workers)
    rec["manifests"][name] = suites.manifest_record(spec)
    rec["sat"] += res["rows"]
    rec["sat_floor"][name] = res["floor"]
    rec["sat_disagree"][name] = res["disagree"]
    rec["budgets"][name] = spec["budget_s"]
    if res["invalid"] and rec["valid"]:
        rec["valid"], rec["reason"] = False, f"{name}: {res['invalid']}"


def _anchor(rec: dict, a):
    if a.anchor == "none":
        return None
    rec["anchor"] = {"tag": a.anchor, "commit": builds.tag_commit(a.anchor)}
    return builds.build_tag(a.anchor, Path(a.build_root))


def _rand(rec: dict, name: str, a) -> None:
    from .vlt_protocol import PROTOCOL
    spec = suites.load(name)
    head = builds.head_bin(a.head_bin)
    anchor = _anchor(rec, a)
    paths = {n: tools.path(n) for n in ("z3", "bitwuzla")}
    arms = run_rand.make_arms(head, anchor, paths)
    res = run_rand.run_suite(spec, arms, a.rand_workers)
    rec["manifests"][name] = {"hash": spec["hash"], "suite": name, "kind": "rand",
                              "protocol": PROTOCOL, "benches": spec["benches"]}
    rec["rand"] += res["rows"]
    if res["invalid"] and rec["valid"]:
        rec["valid"], rec["reason"] = False, f"{name}: {res['invalid']}"


def collect(kind: str, suite_names: list, a=None) -> dict:
    load0 = calib.loadavg()
    utc = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_number = os.environ.get("GITHUB_RUN_NUMBER")
    rec = {
        "schema": schema.SCHEMA, "valid": True, "kind": kind,
        "run": {"utc": utc, "commit": _commit(),
                "ref": os.environ.get("GITHUB_REF", "local"),
                "dv_version": _dv_version(),
                "workflow_run": int(run_number) if run_number else None},
        "tools": calib.tool_versions(),
        "anchor": None,
        "calib": {},
        "manifests": {}, "budgets": {},
        "sat": [], "sat_floor": {}, "sat_disagree": {},
        "rand": [],
    }
    try:
        if "calib" in suite_names:
            rec["calib"] = calib.calibrate()
        if any(n != "calib" for n in suite_names):
            rec["tools"].update(tools.check(["z3", "bitwuzla", "boolector"]))
        for n in suite_names:
            if n.startswith("rand-"):
                _rand(rec, n, a)
            elif n != "calib":
                _sat(rec, n, a)
    except RuntimeError as e:
        rec["valid"], rec["reason"] = False, str(e)
    load1 = calib.loadavg()
    rec["machine"] = dict(calib.machine(), loadavg_start=load0, loadavg_end=load1,
                          noisy=max(load0, load1) > NOISY_LOAD)
    return rec


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--kind", choices=schema.KINDS, default="manual")
    ap.add_argument("--suites", default="calib",
                    help=f"comma-separated, from {', '.join(SUITES)}")
    ap.add_argument("--out", default="perf-out")
    ap.add_argument("--github-output", action="store_true")
    ap.add_argument("--head-bin", help="head dv-solve-smt2 (default build/dv-solve-smt2)")
    ap.add_argument("--anchor", default=builds.ANCHOR, help="anchor release tag, or 'none'")
    ap.add_argument("--build-root", default="perf-builds", help="where tag builds go")
    ap.add_argument("--workers", type=int, default=0, help="0: a quarter of the physical cores")
    ap.add_argument("--rand-workers", type=int, default=1,
                    help="randomization runs one at a time by default: z3's CPU time "
                         "doubles beside parallel neighbours, dv-solve's barely moves, "
                         "so parallel runs would flatter dv-solve")
    a = ap.parse_args(argv)
    suites = [s for s in a.suites.split(",") if s]
    unknown = set(suites) - set(SUITES)
    if unknown:
        ap.error(f"unknown suites: {', '.join(sorted(unknown))}")

    rec = collect(a.kind, suites, a)
    errs = schema.validate(rec)
    if errs:
        print("record fails its own schema:\n  " + "\n  ".join(errs), file=sys.stderr)
        return 2
    name = schema.artifact_name(a.kind, rec["run"]["utc"], rec["run"]["commit"])
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    with gzip.GzipFile(out / f"{name}.json.gz", "wb", mtime=0) as f:
        f.write(json.dumps(rec, indent=1, sort_keys=True).encode())
    print(name)
    if a.github_output and os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as f:
            f.write(f"name={name}\n")
    if not rec["valid"]:
        print(f"INVALID run: {rec['reason']}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
