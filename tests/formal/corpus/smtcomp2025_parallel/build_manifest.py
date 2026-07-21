#!/usr/bin/env python3
"""Regenerate meta/manifest.json + meta/reference_results.csv from the verbatim
SMT-COMP 2025 data snapshots (meta/rp2025.json.gz, meta/bm2025.json.gz).

manifest.json: one entry per bit-vector-family benchmark the parallel track ran,
carrying its logic/family/name, ground-truth status (from the SMT-LIB index),
and each reference solver's competition result (result + cpu + wall).
"""
from __future__ import annotations

import collections
import csv
import gzip
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
META = HERE / "meta"
BV = {"QF_BV", "QF_ABV", "QF_UFBV", "QF_AUFBV"}


def main() -> int:
    res = json.load(gzip.open(META / "rp2025.json.gz"))["results"]
    idx = {}
    for e in json.load(gzip.open(META / "bm2025.json.gz"))["non_incremental"]:
        f = e["file"]
        idx[(f["logic"], f["name"])] = e.get("status")

    bench = collections.OrderedDict()
    for r in res:
        f = r["file"]
        if f["logic"] not in BV:
            continue
        key = (f["logic"], "/".join(f["family"]), f["name"])
        b = bench.setdefault(key, dict(logic=f["logic"], family="/".join(f["family"]),
                                       name=f["name"],
                                       status=idx.get((f["logic"], f["name"])),
                                       solvers={}))
        b["solvers"][r["solver"]] = dict(result=r["result"],
                                         cpu=round(r["cpu_time"], 2),
                                         wall=round(r["wallclock_time"], 2))
    manifest = list(bench.values())
    json.dump(manifest, open(META / "manifest.json", "w"), indent=1)

    solvers = sorted({s for b in manifest for s in b["solvers"]})
    with open(META / "reference_results.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        hdr = ["logic", "family", "name", "status"]
        for s in solvers:
            hdr += [f"{s}_result", f"{s}_wall_s"]
        w.writerow(hdr)
        for b in manifest:
            row = [b["logic"], b["family"], b["name"], b["status"]]
            for s in solvers:
                d = b["solvers"].get(s, {})
                row += [d.get("result", ""), d.get("wall", "")]
            w.writerow(row)

    print(f"manifest: {len(manifest)} benchmarks; reference solvers: {solvers}")
    print("by logic:", dict(collections.Counter(b["logic"] for b in manifest)))
    print("by status:", dict(collections.Counter(b["status"] for b in manifest)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
