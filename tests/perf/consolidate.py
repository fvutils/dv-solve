"""Turn perf artifacts into committed history (design §4.5).

    python3 -m tests.perf.consolidate                # from Forgejo (see history.py env)
    python3 -m tests.perf.consolidate --from-dir D   # from records already on disk

Writes, under tests/perf/history/:
  <year>.jsonl              trend lines appended, kept sorted by utc, deduplicated
  releases/<tag>.json.gz    the full record of a release run (earliest valid wins)
  machines/<id>.json        a machine class the history has not seen before
  manifests/<hash>.json     a resolved suite (fixture order of the ratio arrays)
It never commits: review the diff, run the internal-identifier grep, and
commit it like any other change. Running it twice changes nothing.
"""
from __future__ import annotations

import argparse
import gzip
import json
import sys
from pathlib import Path

from . import history, schema


def _write_jsonl(path: Path, lines: list) -> None:
    lines = sorted(lines, key=schema.line_key)
    path.write_text("".join(json.dumps(l, sort_keys=True, separators=(",", ":")) + "\n"
                            for l in lines))


def consolidate(records: list, root: Path = history.HISTORY) -> dict:
    """records: [(artifact name, run record)]. Returns a summary."""
    summary = {"runs": 0, "lines": 0, "invalid": 0, "noisy": 0, "releases": [],
               "machines": [], "manifests": [], "rejected": [], "range": None}
    by_year = {}
    for p in root.glob("*.jsonl"):
        by_year[p.stem] = {schema.line_key(l): l for l in history.committed_lines_from(p)}
    utcs = []
    for art, rec in records:
        errs = schema.validate(rec)
        if errs:
            summary["rejected"].append(f"{art}: {'; '.join(errs)}")
            continue
        summary["runs"] += 1
        summary["invalid"] += not rec["valid"]
        summary["noisy"] += rec["machine"]["noisy"]
        utcs.append(rec["run"]["utc"])
        for line in schema.trend_lines(rec, art):
            year = by_year.setdefault(line["utc"][:4], {})
            if schema.line_key(line) not in year:
                year[schema.line_key(line)] = line
                summary["lines"] += 1
        m = rec["machine"]
        mp = root / "machines" / f"{m['id']}.json"
        if not mp.exists():
            mp.parent.mkdir(parents=True, exist_ok=True)
            mp.write_text(json.dumps({k: m[k] for k in ("id", "cpu", "cores", "mem_gb", "kernel")},
                                     indent=1, sort_keys=True) + "\n")
            summary["machines"].append(m["id"])
        for man in rec.get("manifests", {}).values():
            mp = root / "manifests" / f"{man['hash']}.json"
            if not mp.exists():
                mp.parent.mkdir(parents=True, exist_ok=True)
                mp.write_text(json.dumps(man, indent=1, sort_keys=True) + "\n")
                summary["manifests"].append(man["hash"])
        ref = rec["run"]["ref"]
        if rec["kind"] == "release" and rec["valid"] and ref.startswith("refs/tags/"):
            tag = ref[len("refs/tags/"):]
            rp = root / "releases" / f"{tag}.json.gz"
            if not rp.exists():        # earliest valid one wins: records arrive oldest first
                rp.parent.mkdir(parents=True, exist_ok=True)
                with gzip.GzipFile(rp, "wb", mtime=0) as f:
                    f.write(json.dumps(rec, sort_keys=True, separators=(",", ":")).encode())
                summary["releases"].append(tag)
    for year, lines in by_year.items():
        if lines:
            _write_jsonl(root / f"{year}.jsonl", list(lines.values()))
    if utcs:
        summary["range"] = (min(utcs), max(utcs))
    return summary


def _from_dir(d: Path) -> list:
    out = []
    for p in sorted(d.glob("*.json.gz")):
        name = p.name[:-len(".json.gz")]
        out.append((name, json.loads(gzip.decompress(p.read_bytes()))))
    return sorted(out, key=lambda r: r[1].get("run", {}).get("utc", ""))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--from-dir", help="consolidate records already downloaded here")
    a = ap.parse_args(argv)
    if a.from_dir:
        records = _from_dir(Path(a.from_dir))
    else:
        fj = history.Forgejo()
        records = []
        try:
            for name, aid in history.unconsolidated(fj.perf_artifacts(), history.committed_lines()):
                records += [(name, rec) for _, rec in fj.records(aid)]
        except history.AccessError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        records.sort(key=lambda r: r[1].get("run", {}).get("utc", ""))
    s = consolidate(records)
    rng = f" ({s['range'][0]} .. {s['range'][1]})" if s["range"] else ""
    print(f"runs: {s['runs']}{rng}; trend lines added: {s['lines']}; "
          f"invalid: {s['invalid']}; noisy: {s['noisy']}")
    for t in s["releases"]:
        print(f"release snapshot written: {t}")
    for m in s["machines"]:
        print(f"new machine class: {m}")
    for r in s["rejected"]:
        print(f"REJECTED {r}", file=sys.stderr)
    if s["lines"] or s["releases"] or s["machines"]:
        print("review `git diff tests/perf/history`, run the identifier grep, then commit")
    return 1 if s["rejected"] else 0


if __name__ == "__main__":
    sys.exit(main())
