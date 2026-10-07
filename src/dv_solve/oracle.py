"""Reports over dv-solve-smt2 oracle-check run directories (DV_ORACLE).

    python -m dv_solve.oracle report <root>... [--out DIR] [--allow-truncated]
    python -m dv_solve.oracle extract <run-dir> <q> [-o FILE]

``report`` finds every run directory (a ``run.json`` of kind
``dv-solve-oracle-run``) under the roots, totals the result classes, lists runs
that did not finish, and groups the non-ok queries by signature. It writes
``oracle-report.md`` and ``oracle-report.json`` (into ``--out``, default the
first root) and exits 1 if any query is ``bad-model`` or ``bad-unsat``, or any
run is truncated or aborted (unless ``--allow-truncated``).

``extract`` cuts query <q> out of a run's ``transcript.smt2``
(``DV_ORACLE_KEEP=all``) as a stand-alone script, passing or not. A failing
query's script is already in the run's ``fail/`` directory.

See docs/oracle_check_plan.md.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter, defaultdict
from pathlib import Path

CLASSES = ["ok", "bad-model", "bad-unsat", "gap", "unchecked", "oracle-error", "skipped"]
P0 = ("bad-model", "bad-unsat")


def find_runs(roots):
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            if "run.json" in filenames:
                p = Path(dirpath)
                try:
                    d = json.loads((p / "run.json").read_text())
                except (OSError, ValueError):
                    continue
                if d.get("kind") == "dv-solve-oracle-run":
                    yield p, d
                dirnames[:] = []    # a run directory holds no other runs


def _pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except (OSError, TypeError):
        return False


def report(roots, out_dir, allow_truncated=False):
    totals = Counter()
    runs = []
    groups = defaultdict(list)       # signature -> [(run, record)]
    for run_dir, rj in find_runs(roots):
        status = rj.get("status")
        counts = Counter()
        n = 0
        qfile = run_dir / "queries.jsonl"
        if qfile.exists():
            with qfile.open() as f:
                for line in f:
                    try:
                        r = json.loads(line)
                    except ValueError:
                        continue      # a line cut short by a kill
                    n += 1
                    counts[r["res"]] += 1
                    if r["res"] not in ("ok", "skipped"):
                        sig = (r["res"], r["cmd"], r["sem"], r["dvs"]["verdict"],
                               r["dvs"]["engine"])
                        groups[sig].append((run_dir, r))
        elif "summary" in rj:     # DV_ORACLE_KEEP=summary: counts only
            summ = rj["summary"]
            n = summ.get("queries", 0)
            counts.update({c: summ[c] for c in CLASSES if summ.get(c)})
        if status == "running" and not _pid_alive(rj.get("pid")):
            status = "truncated"
        totals.update(counts)
        runs.append({"dir": str(run_dir), "tag": rj.get("tag"), "status": status,
                     "queries": n, "counts": dict(counts),
                     "oracle": rj.get("oracle", {}), "dv_solve": rj.get("dv_solve", {}),
                     "dvs_ms": rj.get("summary", {}).get("dvs_ms"),
                     "oracle_ms": rj.get("summary", {}).get("oracle_ms")})

    unfinished = [r for r in runs if r["status"] in ("truncated", "aborted", "failed")]
    p0 = sum(totals[c] for c in P0)
    out_dir.mkdir(parents=True, exist_ok=True)

    js = {"runs": runs, "totals": dict(totals),
          "groups": [{"res": k[0], "cmd": k[1], "sem": k[2], "dvs_verdict": k[3],
                      "engine": k[4], "count": len(v),
                      "examples": [{"run": str(rd), "q": r["q"], "file": r.get("file")}
                                   for rd, r in v[:10]]}
                     for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1]))]}
    (out_dir / "oracle-report.json").write_text(json.dumps(js, indent=1) + "\n")

    L = ["# dv-solve oracle report", ""]
    L.append(f"{len(runs)} runs, {sum(r['queries'] for r in runs)} queries. "
             f"**{p0} P0** (bad-model + bad-unsat); {len(unfinished)} runs unfinished.")
    L += ["", "| class | queries |", "|---|---:|"]
    for c in CLASSES:
        L.append(f"| {c} | {totals.get(c, 0)} |")
    L += ["", "## Runs", "",
          "| run | status | queries | ok | bad-model | bad-unsat | gap | unchecked | oracle-error | dvs s | oracle s |",
          "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for r in sorted(runs, key=lambda r: r["tag"] or r["dir"]):
        c = r["counts"]
        L.append(f"| {r['tag'] or r['dir']} | {r['status']} | {r['queries']} | "
                 + " | ".join(str(c.get(k, 0)) for k in CLASSES[:6])
                 + f" | {(r['dvs_ms'] or 0) / 1000:.1f} | {(r['oracle_ms'] or 0) / 1000:.1f} |")
    if groups:
        L += ["", "## Non-ok queries by signature", "",
              "| result | command | semantics | dvs | engine | count | first example |",
              "|---|---|---|---|---|---:|---|"]
        for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            rd, r = v[0]
            ex = f"{rd}/{r['file']}.smt2" if r.get("file") else f"{rd} q{r['q']}"
            L.append(f"| {k[0]} | {k[1]} | {k[2]} | {k[3]} | {k[4]} | {len(v)} | `{ex}` |")
    (out_dir / "oracle-report.md").write_text("\n".join(L) + "\n")
    print("\n".join(L[:14 + len(CLASSES)]))
    print(f"\nwrote {out_dir / 'oracle-report.md'}")
    if p0 or (unfinished and not allow_truncated):
        return 1
    return 0


def extract(run_dir, q, out):
    t = Path(run_dir) / "transcript.smt2"
    if not t.exists():
        sys.exit(f"{t}: no transcript (the run needs DV_ORACLE_KEEP=all)")
    start, end = f"; @q {q} dvs=", f"; @q {q} orc="
    lines, on = [], False
    with t.open() as f:
        for line in f:
            if line.startswith(start):
                on = True
                lines.append(line)
                continue
            if on and line.startswith(end):
                lines.append(line)
                break
            if on and not (line.strip() == "(reset)" and len(lines) == 1):
                lines.append(line)
    if not lines:
        sys.exit(f"query {q} not in {t}")
    text = "".join(lines)
    if out:
        Path(out).write_text(text)
    else:
        sys.stdout.write(text)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m dv_solve.oracle")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("report")
    r.add_argument("roots", nargs="+")
    r.add_argument("--out", default=None)
    r.add_argument("--allow-truncated", action="store_true")
    e = sub.add_parser("extract")
    e.add_argument("run_dir")
    e.add_argument("q", type=int)
    e.add_argument("-o", "--out", default=None)
    a = ap.parse_args(argv)
    if a.cmd == "report":
        return report(a.roots, Path(a.out or a.roots[0]), a.allow_truncated)
    extract(a.run_dir, a.q, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
