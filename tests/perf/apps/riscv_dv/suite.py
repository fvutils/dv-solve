"""The riscv-dv application suite: every cell under every arm, one record.

Per cell, in order: the timed reps of each arm (interleaved across reps),
then each arm's recorded run. A dv arm's recorded run is an extra, untimed
run; a reference arm's recorded run is also its timed run (§5.2 of
docs/riscv_dv_results_design.md). Everything runs serially: load inflated
the T2 sweep's times 2.5x.

Fills rec["app"] (one row per cell and arm), rec["app_summary"][suite]
(scores, value and program diversity per arm) and the suite's manifest. The
run is invalid if a cell's options did not take effect, or a dv arm fails a
cell the reference passes (§6.2). A dv arm whose value spread falls below the
reference's marks the summary `degraded` (§6.3). Different programs from one
seed across a dv arm's runs are reported (`deterministic: false`), not
invalid: dv-solve's per-shape engine choice depends on measured solve times,
so a model can legitimately differ between runs.
"""
from __future__ import annotations

import collections
import hashlib
import json
import math
import os
import shutil
import time
from pathlib import Path

from . import build, dist, quality, run

_HERE = Path(__file__).resolve().parent
_SUITES = _HERE.parents[1] / "suites"
NOISY_CELL_LOAD = 2.0   # host 1-min load at the start of any of a cell's runs


def load(name: str) -> dict:
    """The suite spec. $DVS_APP_CELLS (comma-separated target/test) keeps only
    those cells, for local runs; the manifest then says `subset`."""
    spec = json.loads((_SUITES / f"{name}.json").read_text())
    want = os.environ.get("DVS_APP_CELLS")
    if want:
        keep = set(want.split(","))
        spec["cells"] = [c for c in spec["cells"] if f"{c['target']}/{c['test']}" in keep]
        if not spec["cells"]:
            raise RuntimeError(f"DVS_APP_CELLS={want} matches no cell of {name}")
        spec["subset"] = True
    return spec


def manifest(spec: dict, inputs: build.Inputs) -> dict:
    """The resolved manifest: cells plus every build input (§4.2)."""
    cells = [{k: c[k] for k in ("target", "test", "gen_test", "seed", "plusargs")}
             for c in spec["cells"]]
    key = json.dumps([cells, inputs.record()], sort_keys=True)
    return {"suite": spec["suite"], "kind": "app", "app": spec["app"],
            "hash": hashlib.sha256(key.encode()).hexdigest()[:12],
            "inputs": inputs.record(), "cells": cells,
            "arms": spec["arms"], "reference": spec["reference"],
            "subset": spec.get("subset", False)}


def _solver_cmd(arm: dict, head_bin: Path, z3: Path) -> list:
    if arm["solver"] == "dv":
        return [head_bin, "--interactive", "--mode=verilator"]
    if arm["solver"] == "z3":
        return [z3, "-in"]
    raise RuntimeError(f"unknown solver {arm['solver']!r}")


def _settle(log, below: float = 1.0, max_s: float = 120.0) -> None:
    """Let a generator build's load (make -j32) decay before anything is timed."""
    from ... import calib
    t0 = time.monotonic()
    while calib.loadavg() > below and time.monotonic() - t0 < max_s:
        time.sleep(5)
    log(f"load {calib.loadavg():.2f} after {time.monotonic() - t0:.0f} s settling")


def _geomean(xs):
    xs = [x for x in xs if x and x > 0]
    return round(math.exp(sum(map(math.log, xs)) / len(xs)), 3) if xs else None


def collect(rec: dict, name: str, head_bin: Path, z3: Path, inputs: build.Inputs,
            workroot: Path, log=print) -> None:
    spec = load(name)
    man = manifest(spec, inputs)
    rec.setdefault("manifests", {})[name] = man
    rec.setdefault("app_inputs", {})[name] = man["inputs"]
    tools = inputs.tools()
    gens = {}
    for t in sorted({c["target"] for c in spec["cells"]}):
        log(f"[{name}] generator {t}: building or reusing")
        gens[t] = inputs.generator(t)
    rec.setdefault("app_gen_sha", {})[name] = {t: build.sha256_file(p)[:16] for t, p in gens.items()}
    _settle(log)

    arms = spec["arms"]
    ref = spec["reference"]
    rows = []
    counts = {a["name"]: collections.defaultdict(collections.Counter) for a in arms}
    progs = {a["name"]: [] for a in arms}
    for c in spec["cells"]:
        cid = f"{c['target']}/{c['test']}/s{c['seed']}"
        cwork = workroot / name / cid.replace("/", "_")
        best = {}
        reps = collections.defaultdict(list)
        loads = collections.defaultdict(list)
        shas = collections.defaultdict(set)
        max_reps = max(a["reps"] for a in arms)
        for rep in range(max_reps):
            for a in arms:
                if rep >= a["reps"] or a["recorded"] == "timed":
                    continue
                r = run.run_cell(gens[c["target"]], c, _solver_cmd(a, head_bin, z3), a["sampler"],
                                 "timed", tools, cwork / a["name"].replace("@", "_"),
                                 spec["timeout_s"])
                log(f"[{name}] {cid} {a['name']} timed rep {rep + 1}: {r['outcome']} "
                    f"wall {r['wall_s']:.1f}s solver {r['solver_cpu_s']:.1f}s")
                if r.get("prog_sha"):
                    shas[a["name"]].add(r["prog_sha"])
                b = best.get(a["name"])
                if b is None or (r["outcome"] == "pass" and
                                 (b["outcome"] != "pass" or r["solver_cpu_s"] < b["solver_cpu_s"])):
                    best[a["name"]] = r
                reps[a["name"]].append(r["solver_cpu_s"])
                loads[a["name"]].append(r["load"])
        for a in arms:
            r = run.run_cell(gens[c["target"]], c, _solver_cmd(a, head_bin, z3), a["sampler"],
                             "recorded", tools, cwork / (a["name"].replace("@", "_") + "_rec"),
                             spec["timeout_s"])
            log(f"[{name}] {cid} {a['name']} recorded: {r['outcome']} "
                f"wall {r['wall_s']:.1f}s solver {r['solver_cpu_s']:.1f}s")
            if r["_transcript"] and r["_transcript"].exists():
                dist.reduce(r["_transcript"], counts[a["name"]])
            if r.get("prog_sha"):
                shas[a["name"]].add(r["prog_sha"])
            if a["recorded"] == "timed":
                reps[a["name"]].append(r["solver_cpu_s"])
                loads[a["name"]].append(r["load"])
                best[a["name"]] = r
            else:
                best[a["name"]]["calls"] = r.get("calls")
                if r["outcome"] != "pass" and best[a["name"]]["outcome"] == "pass":
                    best[a["name"]]["recorded_outcome"] = r["outcome"]
        for a in arms:
            r = best[a["name"]]
            if r.get("_prog"):
                progs[a["name"]].append(r["_prog"])
            row = {"suite": name, "target": c["target"], "test": c["test"], "seed": c["seed"],
                   "arm": a["name"], "build": "head",
                   **{k: v for k, v in r.items() if not k.startswith("_")},
                   "solver_cpu_reps": reps[a["name"]],
                   "noisy": max(loads[a["name"]], default=0) > NOISY_CELL_LOAD,
                   "deterministic": len(shas[a["name"]]) <= 1,
                   "quality": quality.public(r["_prog"]) if r.get("_prog") else None}
            rows.append(row)
        shutil.rmtree(cwork, ignore_errors=True)
    rec.setdefault("app", []).extend(rows)
    rec.setdefault("app_summary", {})[name] = summarize(spec, rows, counts, progs)
    _validate(rec, name, spec, rows)


def summarize(spec: dict, rows: list, counts: dict, progs: dict) -> dict:
    arms = [a["name"] for a in spec["arms"]]
    ref = spec["reference"]
    by = {(r["target"], r["test"], r["seed"], r["arm"]): r for r in rows}
    cells = [(c["target"], c["test"], c["seed"]) for c in spec["cells"]]
    out = {"reference": ref, "arms": {}, "ratios": {}}
    ref_pool = quality.pooled(progs[ref])
    vd = dist.summary(counts, arms, ref)
    for a in arms:
        mine = [by[c + (a,)] for c in cells]
        ok = [r for r in mine if r["outcome"] == "pass"]
        out["arms"][a] = {
            "pass": len(ok), "cells": len(mine),
            "wall_s": round(sum(r["wall_s"] for r in ok), 2),
            "solver_cpu_s": round(sum(r["solver_cpu_s"] for r in ok), 2),
            "sim_cpu_s": round(sum(r["sim_cpu_s"] for r in ok), 2),
            "deterministic": all(r["deterministic"] for r in mine),
            "noisy_cells": sum(1 for r in mine if r.get("noisy")),
            "program": quality.public(quality.pooled(progs[a], ref_pool["_mn"])),
            "values": vd.get(a),
        }
    for a in arms:
        if a == ref:
            continue
        e2e, cpu = [], []
        for c in cells:
            x, y = by[c + (a,)], by[c + (ref,)]
            both = x["outcome"] == "pass" and y["outcome"] == "pass"
            e2e.append(round(y["wall_s"] / x["wall_s"], 3) if both else None)
            cpu.append(round(y["solver_cpu_s"] / x["solver_cpu_s"], 3)
                       if both and x["solver_cpu_s"] > 0 else None)
        out["ratios"][a] = {"e2e_speedup": e2e, "cpu_speedup": cpu,
                            "e2e_geomean": _geomean(e2e), "cpu_geomean": _geomean(cpu)}
    rv = (out["arms"][ref]["values"] or {}).get("hhmax")
    out["degraded"] = sorted(a for a in arms if a != ref and rv is not None and
                             (out["arms"][a]["values"] or {}).get("hhmax") is not None and
                             out["arms"][a]["values"]["hhmax"] < rv)
    return out


def _validate(rec: dict, name: str, spec: dict, rows: list) -> None:
    ref = spec["reference"]
    bad = []
    for r in rows:
        if r.get("options_ok") is False:
            bad.append(f"{r['target']}/{r['test']} {r['arm']}: options did not take effect "
                       f"(program below {r.get('options_floor')} instructions)")
    by = {(r["target"], r["test"], r["seed"], r["arm"]): r for r in rows}
    for r in rows:
        if r["arm"] == ref or not r["arm"].startswith("dv@"):
            continue
        rr = by.get((r["target"], r["test"], r["seed"], ref))
        if rr and rr["outcome"] == "pass" and r["outcome"] != "pass":
            bad.append(f"{r['target']}/{r['test']} {r['arm']}: {r['outcome']} where {ref} "
                       f"passes ({r.get('signature')})")
    if bad and rec.get("valid", True):
        rec["valid"] = False
        rec["reason"] = f"{name}: " + "; ".join(bad[:5]) + (f" (+{len(bad) - 5} more)" if len(bad) > 5 else "")
