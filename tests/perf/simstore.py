"""simbench run records kept for good on the bench-data branch (docs/simbench_nightly_plan.md §6a).

    python3 -m tests.perf.simstore add --root DIR [--from-dir D]
    python3 -m tests.perf.simstore kind --event E [--input KIND] --verilator VER [--sims FILE]

`add` copies every unexpired simbench-<nightly|reference|release>-* artifact
(Forgejo, as tests.perf.history reads perf's) that DIR does not hold yet into
DIR/records/, then rebuilds everything derived from the records:

  records/<yyyy>/<artifact>.json.gz   each run's record, as uploaded
  history/<yyyy>.jsonl                one trend line per (run, variant), sorted by utc
  manifests/<hash>.json               the bench order of a manifest's per-bench arrays
  sims.json                           variant -> simulator identity -> first/last utc, runs

The derived files are rebuilt from records/ every time, so running it twice
changes nothing, and a format change needs no migration. It prints the
artifacts it added (one per line) and never commits: the workflow's record job
does, after the internal-identifier check. Manual runs are not kept.

`kind` picks a scheduled night's kind: Sundays, and any night whose Verilator
has no z3 reference yet, are `reference`; others `nightly`. A dispatch keeps
the kind it was given.
"""
from __future__ import annotations

import argparse
import datetime
import gzip
import json
import re
import sys
from pathlib import Path

ARTIFACT_RE = re.compile(r"^simbench-(?P<kind>nightly|reference|release)-"
                         r"(?P<utc>\d{8}T\d{6}Z)-(?P<sha>[0-9a-f]{7,40})$")
OUT = {"ok": "o", "wrong": "w", "failed": "f", "unsupported": "u", "error": "e"}
# The simulator each variant runs, by `tests.perf.tools latest` tool name.
SIM_TOOL = {"vlt-dvs-smt": "verilator-sc", "dvs-standalone": "verilator-sc",
            "vlt-z3-default": "verilator-sc", "vlt-dvs-lib": "verilator-sc", "xezim": "xezim"}
REFERENCE = "vlt-z3-default"


def _r(x, nd=4):
    return None if x is None else round(x, nd)


def manifest_of(rec: dict) -> list:
    """[[bench, sha12]] in manifest order. Records before 2026-11-05 carry no
    sim.benches; their order is the sorted union of bench rows, without shas."""
    if rec["sim"].get("benches"):
        return rec["sim"]["benches"]
    names = {b["bench"] for blk in rec["sim"]["variants"].values() for b in blk.get("benches", [])}
    return [[n, None] for n in sorted(names)]


def _noisy(rec: dict):
    if "noisy" in rec["sim"]:
        return rec["sim"]["noisy"]
    waits = rec["sim"].get("quiet_waits")
    return any(not w["quiet"] for w in waits.values()) if waits else None


def trend_lines(rec: dict, name: str) -> list:
    """One line per (run, variant) that ran (plan §6)."""
    order = [b for b, _ in manifest_of(rec)]
    m = rec["sim"].get("manifest")
    sims = (rec.get("tools") or {}).get("sims") or {}
    lines = []
    for variant, blk in sorted(rec["sim"]["variants"].items()):
        if "benches" not in blk:
            continue
        rows = {b["bench"]: b for b in blk["benches"]}

        def col(f):
            return [f(rows[b]) if b in rows else None for b in order]
        spread = lambda b: b.get("spread") or {}
        lines.append({
            "h": 1, "utc": rec["run"]["utc"], "kind": rec["kind"], "artifact": name,
            "variant": variant, "sim": blk.get("sim"),
            "src": (sims.get(SIM_TOOL.get(variant, "")) or {}).get("src"),
            "dvs": (blk.get("dvs") or {}).get("version") or (blk.get("dvs") or {}).get("commit"),
            "commit": (rec["run"].get("commit") or "")[:12], "run": rec["run"].get("workflow_run"),
            "m": m, "valid": rec["valid"], "noisy": _noisy(rec),
            "geo_us": blk.get("geo_us"), "counts": blk.get("counts"),
            # one letter per bench: o ok, w wrong, f failed, u unsupported, e error, - not run
            "out": "".join(OUT.get(rows[b]["outcome"], "e") if b in rows else "-" for b in order),
            "us": col(lambda b: b.get("us") if b["outcome"] == "ok" else None),
            "ses": col(lambda b: b.get("ses")),
            "ent": col(lambda b: _r(spread(b).get("min_entropy"))),
            "dist": col(lambda b: _r(spread(b).get("min_distinct"))),
            "stuck": col(lambda b: spread(b).get("stuck")),
            "chi2": col(lambda b: _r(spread(b).get("chi2_max"), 3)),
        })
    return lines


def _year(utc: str) -> str:
    return utc[:4]


def _dump(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=1, sort_keys=True) + "\n", encoding="utf-8")


def write_record(root: Path, name: str, rec: dict) -> Path:
    p = root / "records" / _year(ARTIFACT_RE.match(name)["utc"]) / (name + ".json.gz")
    p.parent.mkdir(parents=True, exist_ok=True)
    # mtime=0: the same record always gives the same bytes
    with gzip.GzipFile(p, "wb", mtime=0) as fp:
        fp.write(json.dumps(rec, sort_keys=True, separators=(",", ":")).encode())
    return p


def held(root: Path) -> set:
    return {p.name[:-len(".json.gz")] for p in (root / "records").glob("*/*.json.gz")}


def rebuild(root: Path) -> None:
    by_year, manifests, sims = {}, {}, {}
    for p in sorted((root / "records").glob("*/*.json.gz")):
        name = p.name[:-len(".json.gz")]
        rec = json.loads(gzip.decompress(p.read_bytes()))
        for ln in trend_lines(rec, name):
            by_year.setdefault(_year(ln["utc"]), []).append(ln)
            if ln["m"]:
                manifests.setdefault(ln["m"], manifest_of(rec))
            if ln["sim"] and ln["valid"]:
                s = sims.setdefault(ln["variant"], {}).setdefault(
                    ln["sim"], {"first": ln["utc"], "last": ln["utc"], "runs": 0})
                s["first"], s["last"] = min(s["first"], ln["utc"]), max(s["last"], ln["utc"])
                s["runs"] += 1
    for year, lines in by_year.items():
        lines.sort(key=lambda l: (l["utc"], l["variant"], l["artifact"]))
        p = root / "history" / f"{year}.jsonl"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("".join(json.dumps(l, sort_keys=True, separators=(",", ":")) + "\n"
                             for l in lines), encoding="utf-8")
    for h, benches in manifests.items():
        _dump(root / "manifests" / f"{h}.json", benches)
    _dump(root / "sims.json", sims)


def add(root: Path, source) -> list:
    """source: iterable of (artifact name, record). Returns the names added."""
    have, added = held(root), []
    for name, rec in source:
        if name in have or not ARTIFACT_RE.match(name):
            continue
        write_record(root, name, rec)
        have.add(name)
        added.append(name)
    rebuild(root)
    return sorted(added)


def from_dir(d: Path):
    for p in sorted(Path(d).rglob("simbench-*.json.gz")):
        yield p.name[:-len(".json.gz")], json.loads(gzip.decompress(p.read_bytes()))


def from_forgejo(have: set):
    from .history import Forgejo
    fj = Forgejo()
    for name, aid in fj.artifacts(ARTIFACT_RE):
        if name in have:
            continue
        for fname, rec in fj.records(aid):
            yield Path(fname).name[:-len(".json.gz")], rec


def choose_kind(event: str, given: str, weekday: int, verilator: str, sims: dict) -> str:
    """A dispatch keeps its kind. A scheduled night is `reference` on Sundays
    (weekday 6) and whenever its Verilator has no valid z3 reference yet (plan
    §3); otherwise `nightly`."""
    if event != "schedule":
        return given or "manual"
    if weekday == 6 or f"verilator@{verilator}" not in sims.get(REFERENCE, {}):
        return "reference"
    return "nightly"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a_p = sub.add_parser("add")
    a_p.add_argument("--root", required=True, help="the bench-data checkout's simbench/ directory")
    a_p.add_argument("--from-dir", default=None, help="records on disk instead of Forgejo")
    k_p = sub.add_parser("kind")
    k_p.add_argument("--event", required=True)
    k_p.add_argument("--input", default="")
    k_p.add_argument("--verilator", required=True, help="the version this night runs")
    k_p.add_argument("--sims", default=None, help="bench-data's simbench/sims.json, if any")
    a = ap.parse_args(argv)
    if a.cmd == "kind":
        sims = {}
        if a.sims and Path(a.sims).exists() and Path(a.sims).stat().st_size:
            sims = json.loads(Path(a.sims).read_text(encoding="utf-8"))
        print(choose_kind(a.event, a.input, datetime.datetime.now(datetime.timezone.utc).weekday(),
                          a.verilator, sims))
        return 0
    root = Path(a.root)
    src = from_dir(Path(a.from_dir)) if a.from_dir else from_forgejo(held(root))
    for name in add(root, src):
        print(name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
