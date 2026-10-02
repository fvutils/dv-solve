"""Run the perf suites and write one gzipped run record (design §6.1).

    python3 -m tests.perf.collect --kind manual --suites calib --out perf-out

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

from . import calib, schema

SUITES = ("calib",)          # grows with R1+ (sat-core, rand-core, ...)
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


def collect(kind: str, suites: list) -> dict:
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
    }
    try:
        if "calib" in suites:
            rec["calib"] = calib.calibrate()
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
    a = ap.parse_args(argv)
    suites = [s for s in a.suites.split(",") if s]
    unknown = set(suites) - set(SUITES)
    if unknown:
        ap.error(f"unknown suites: {', '.join(sorted(unknown))}")

    rec = collect(a.kind, suites)
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
