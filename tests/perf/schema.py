"""Run records and trend lines (design §4.3, §4.4).

A run record is the full result of one perf job. A trend line is the compact
per-(run, build) summary that is committed to tests/perf/history/. Validation
is hand-written so nothing beyond the standard library is needed in CI.
"""
from __future__ import annotations

import re

SCHEMA = 1          # run record
TREND = 1           # trend line ("h")
KINDS = ("nightly", "release", "ladder", "manual")
# perf-<kind>-<utc>-<shortsha>; the utc sorts, so it orders artifacts too.
ARTIFACT_RE = re.compile(r"^perf-(?P<kind>[a-z]+)-(?P<utc>\d{8}T\d{6}Z)-(?P<sha>[0-9a-f]{7,40})$")

_REQUIRED = {
    "schema": int, "valid": bool, "kind": str, "run": dict, "machine": dict,
    "tools": dict, "calib": dict,
}
_RUN = {"utc": str, "commit": str, "ref": str}
_MACHINE = {"id": str, "cpu": str, "cores": int, "mem_gb": int, "kernel": str,
            "loadavg_start": float, "loadavg_end": float, "noisy": bool}


def artifact_name(kind: str, utc: str, commit: str) -> str:
    return f"perf-{kind}-{utc}-{commit[:7]}"


def validate(rec: dict) -> list:
    """Problems with a run record; empty when it is well formed."""
    errs = []

    def need(obj, spec, where):
        for k, t in spec.items():
            if k not in obj:
                errs.append(f"{where}.{k} missing")
            elif t is float and isinstance(obj[k], (int, float)) and not isinstance(obj[k], bool):
                continue
            elif not isinstance(obj[k], t):
                errs.append(f"{where}.{k} is {type(obj[k]).__name__}, want {t.__name__}")

    if not isinstance(rec, dict):
        return ["record is not an object"]
    need(rec, _REQUIRED, "record")
    if errs:
        return errs
    if rec["schema"] != SCHEMA:
        errs.append(f"unknown schema {rec['schema']}")
    if rec["kind"] not in KINDS:
        errs.append(f"unknown kind {rec['kind']!r}")
    need(rec["run"], _RUN, "run")
    need(rec["machine"], _MACHINE, "machine")
    if not re.fullmatch(r"\d{8}T\d{6}Z", rec["run"].get("utc", "")):
        errs.append("run.utc not in YYYYMMDDTHHMMSSZ form")
    if not rec["valid"] and not rec.get("reason"):
        errs.append("invalid record without a reason")
    return errs


def trend_lines(rec: dict, artifact: str) -> list:
    """The committed summary of a run: one line per build measured in it.

    Calibration belongs to the run; SAT suites add per-fixture ratios of the
    head build against the anchor and each reference, aligned to the suite's
    manifest (design §4.3). Ladder runs will add one line per release build.
    """
    from . import normalize
    m = rec["machine"]
    line = {
        "h": TREND, "utc": rec["run"]["utc"], "commit": rec["run"]["commit"],
        "kind": rec["kind"], "build": "head", "valid": rec["valid"],
        "noisy": m["noisy"], "machine": m["id"],
        "calib": rec["calib"].get("kernel_cpu_ms", {}),
        "anchor": (rec.get("anchor") or {}).get("tag"),
        "lock": rec["tools"].get("lock_sha"), "artifact": artifact,
    }
    if not rec["valid"]:
        line["reason"] = rec.get("reason", "")
    sat = {}
    for name, man in rec.get("manifests", {}).items():
        if not any(r["suite"] == name for r in rec.get("sat", [])):
            continue
        keys = sorted({f"{r['arm']}@{r['build']}" for r in rec["sat"] if r["suite"] == name})
        sat[name] = {
            "m": man["hash"],
            "vs": {k: normalize.ratios(rec, name, k) for k in keys if k != normalize.HEAD},
            "solved": normalize.solved(rec, name),
            "par2_s": normalize.par2(rec, name, rec["budgets"][name]),
            "floor_ms": rec["sat_floor"].get(name, {}),
            "disagree": len(rec.get("sat_disagree", {}).get(name, [])),
        }
    if sat:
        line["sat"] = sat
    rand = {}
    for name, man in rec.get("manifests", {}).items():
        if man.get("kind") != "rand":
            continue
        rows = [r for r in rec.get("rand", []) if "error" not in r]
        uni = {r["bench"]: r["jsd"] for r in rows if r["arm"] == "uniform"}
        # per benchmark and arm: [CPU ms per call, excess JSD, coverage]
        rand[name] = {"m": man["hash"], "protocol": man.get("protocol"), "b": {}}
        for r in rows:
            if r["arm"] == "uniform" or r["bench"] not in uni:
                continue
            rand[name]["b"].setdefault(r["bench"], {})[r["arm"]] = [
                round(r["cpu_ms"], 4) if r.get("cpu_ms") is not None else None,
                round(max(0.0, r["jsd"] - uni[r["bench"]]), 4), round(r["cov"], 4)]
        rand[name]["errors"] = sum(1 for r in rec.get("rand", []) if "error" in r)
    if rand:
        line["rand"] = rand
    return [line]


def line_key(line: dict) -> tuple:
    return (line["utc"], line["commit"], line["kind"], line["build"])
