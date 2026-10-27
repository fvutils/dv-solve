"""Merge simbench JSONL files (from one or many machines) into a markdown table.

    python3 -m dv_solve_simbench.report a.jsonl b.jsonl [--ref vlt-z3-default] [-o table.md]

Rows are benches, columns are `config@host` (host = machine label from SIMBENCH_HOST, else the
machine id). Each cell is us/call, plus the ratio to the reference config on the same host
(or, if that host has none, on the first host that does). Invalid runs show their reason in
short; the last record wins when a (config, host, bench) repeats. The final row is the geomean
of the ratios over benches where both sides are valid.
"""
import argparse
import math
import sys
from typing import Dict, List, Optional, Tuple

from . import records

Key = Tuple[str, str]               # (config, host)


def host_of(rec: dict) -> str:
    m = rec.get("machine") or {}
    return m.get("label") or m.get("id") or "?"


def us_per_call(rec: dict) -> Optional[float]:
    r = (rec.get("simbench") or {}).get("result")
    if not rec.get("valid") or not r or r.get("ns_per_call") is None:
        return None
    return r["ns_per_call"] / 1000.0


def collect(recs: List[dict]) -> Tuple[List[Key], List[str], Dict[Tuple[Key, str], dict]]:
    cells: Dict[Tuple[Key, str], dict] = {}
    cols: List[Key] = []
    benches = set()
    for rec in recs:
        sb = rec.get("simbench")
        if not sb:
            continue
        key = (sb["config"], host_of(rec))
        if key not in cols:
            cols.append(key)
        benches.add(sb["bench"])
        cells[(key, sb["bench"])] = rec
    return cols, sorted(benches), cells


def ref_column(cols: List[Key], col: Key, ref: str) -> Optional[Key]:
    if (ref, col[1]) in cols:
        return (ref, col[1])
    return next((c for c in cols if c[0] == ref), None)


def table(recs: List[dict], ref: str = "vlt-z3-default") -> str:
    cols, benches, cells = collect(recs)
    if not cols:
        return "(no simbench records)\n"
    # Reference columns first, then the rest in input order.
    cols = [c for c in cols if c[0] == ref] + [c for c in cols if c[0] != ref]
    lines = ["| bench | " + " | ".join("%s@%s" % c for c in cols) + " |",
             "|---|" + "---|" * len(cols)]
    logs: Dict[Key, List[float]] = {c: [] for c in cols}
    for b in benches:
        row = [b]
        for c in cols:
            rec = cells.get((c, b))
            if rec is None:
                row.append("-")
                continue
            us = us_per_call(rec)
            if us is None:
                row.append("FAIL: " + (rec.get("reason") or "?")[:30])
                continue
            cell = "%.1f" % us
            rc = ref_column(cols, c, ref)
            rrec = cells.get((rc, b)) if rc and rc != c else None
            rus = us_per_call(rrec) if rrec else None
            if rus:
                ratio = us / rus
                logs[c].append(math.log(ratio))
                cell += " (%.2f)" % ratio
            row.append(cell)
        lines.append("| " + " | ".join(row) + " |")
    geo = ["**geomean ratio vs %s**" % ref]
    for c in cols:
        geo.append("%.3f (n=%d)" % (math.exp(sum(logs[c]) / len(logs[c])), len(logs[c]))
                   if logs[c] else "-")
    lines.append("| " + " | ".join(geo) + " |")
    hosts = {}
    for rec in recs:
        m = rec.get("machine") or {}
        hosts.setdefault(host_of(rec), "%s, %s cores, %s GB, kernel %s" % (
            m.get("cpu"), m.get("cores"), m.get("mem_gb"), m.get("kernel")))
    lines.append("")
    lines.append("Cells: us/call (ratio vs %s on the same host). Hosts:" % ref)
    lines += ["- %s: %s" % kv for kv in sorted(hosts.items())]
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("jsonl", nargs="+")
    ap.add_argument("--ref", default="vlt-z3-default", help="reference config id")
    ap.add_argument("-o", "--out", help="write the table here instead of stdout")
    a = ap.parse_args(argv)
    recs = [r for p in a.jsonl for r in records.load(p)]
    text = table(recs, a.ref)
    if a.out:
        with open(a.out, "w") as fp:
            fp.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
