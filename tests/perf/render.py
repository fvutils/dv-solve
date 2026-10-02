"""Render the docs "Results" pages from perf data (design §7).

    python3 -m tests.perf.render [--records DIR] [--out docs/site/results]

Inputs: the committed history (tests/perf/history/) and any full run records
in --records (fetched artifacts: `python3 -m tests.perf.history fetch`). The
"current" numbers come from the newest valid run record that has SAT data.
Writes index.md and sat.md plus _gen/*.svg and _gen/*.csv; methodology.md is
hand-written and tracked. With no SAT record at all, writes placeholder pages
so the site still builds.
"""
from __future__ import annotations

import argparse
import csv
import gzip
import io
import json
import math
import sys
from pathlib import Path

from . import history, normalize, svg

_REPO = Path(__file__).resolve().parents[2]
SUITE = "sat-core"
REPO_URL = "https://github.com/fvutils/dv-solve"
HEAD = normalize.HEAD
# arm key -> (display name, tool key for the version)
ARMS = {
    "dv-smt2@head": ("dv-solve", None),
    "dv-smt2-bb@head": ("dv-solve (bit-blast)", None),
    "dv-smt2@anchor": ("dv-solve {anchor}", None),
    "bitwuzla@ref": ("bitwuzla {v}", "bitwuzla"),
    "z3@ref": ("z3 {v}", "z3"),
    "boolector@ref": ("boolector {v}", "boolector"),
}
COMPARE = ["bitwuzla@ref", "z3@ref", "dv-smt2@anchor"]


def _name(rec: dict, key: str) -> str:
    fmt, tool = ARMS.get(key, (key, None))
    return fmt.format(anchor=(rec.get("anchor") or {}).get("tag", "anchor"),
                      v=rec["tools"].get(tool, "") if tool else "").strip()


def load_records(d) -> list:
    out = []
    if d:
        for p in sorted(Path(d).glob("*.json.gz")):
            out.append(json.loads(gzip.decompress(p.read_bytes())))
    for p in sorted((history.HISTORY / "releases").glob("*.json.gz")):
        out.append(json.loads(gzip.decompress(p.read_bytes())))
    return out


def current(records: list):
    ok = [r for r in records if r.get("valid") and any(s["suite"] == SUITE for s in r.get("sat", []))]
    return max(ok, key=lambda r: r["run"]["utc"]) if ok else None


def _when(utc: str) -> str:
    return f"{utc[0:4]}-{utc[4:6]}-{utc[6:8]} {utc[9:11]}:{utc[11:13]} UTC"


def provenance(rec: dict) -> str:
    run, m, t = rec["run"], rec["machine"], rec["tools"]
    sha = run["commit"]
    lines = [
        "```{list-table}",
        ":widths: 25 75",
        "",
        f"* - Measured\n  - {_when(run['utc'])}, {rec['kind']} run"
        + (" (host was busy: treat timings with caution)" if m["noisy"] else ""),
        f"* - dv-solve\n  - commit [`{sha[:9]}`]({REPO_URL}/commit/{sha})",
        f"* - Baseline build\n  - dv-solve {(rec.get('anchor') or {}).get('tag', 'none')}, built from its tag",
        "* - Reference solvers\n  - " + ", ".join(f"{k} {t[k]}" for k in ("bitwuzla", "z3", "boolector") if t.get(k)),
        f"* - Machine\n  - {m['cpu']}, {m['cores']} logical CPUs, {m['mem_gb']} GB, Linux {m['kernel']}",
        "```",
    ]
    return "\n".join(lines)


def _cat_score(rat, fixtures, cat):
    xs = [r for r, f in zip(rat, fixtures) if r is not None and f["cat"] == cat]
    return (math.exp(sum(xs) / len(xs) / 1000), len(xs)) if xs else (None, 0)


def _factor(x) -> str:
    if x is None:
        return "–"
    return f"{x:.2f}×" if x < 10 else f"{x:.0f}×"


def _ms(x) -> str:
    if x is None:
        return "–"
    return f"{x:.1f}" if x < 100 else f"{x:.0f}"


def summary(rec: dict) -> dict:
    man = rec["manifests"][SUITE]
    out = {"n": len(man["fixtures"]), "solved": normalize.solved(rec, SUITE),
           "floor": rec.get("sat_floor", {}).get(SUITE, {}), "scores": {}}
    for k in COMPARE:
        rat = normalize.ratios(rec, SUITE, k)
        out["scores"][k] = (normalize.score(rat, man["fixtures"], man["weights"]),
                            sum(r is not None for r in rat))
    dis = rec.get("sat_disagree", {}).get(SUITE, [])
    out["wrong_head"] = [d for d in dis if d["kind"].endswith("@head")]
    out["wrong_anchor"] = [d for d in dis if d["kind"] == "dv-smt2@anchor"]
    out["ref_split"] = [d for d in dis if d["kind"] == "references"]
    return out


PLACEHOLDER = """# {title}

No performance run with SAT results has been recorded yet. This page fills in
after the first nightly performance run; see {{doc}}`methodology` for what
it will show and how the numbers are produced.
"""


def page_index(rec: dict, s: dict) -> str:
    L = ["# Results", "",
         "How dv-solve compares, as an SMT-LIB2 solver, with the reference solvers "
         "bitwuzla and z3 on the same machine in the same run. The numbers are "
         "regenerated from the latest nightly performance run; "
         "{doc}`methodology` explains how each one is produced.", "",
         provenance(rec), "",
         "## At a glance", ""]
    L += [f"| Solver | Answered correctly (of {s['n']}) | Start-up, CPU ms | dv-solve speed-up | Wrong answers |",
          "|---|---|---|---|---|"]
    wrong = {HEAD: len(s["wrong_head"]), "dv-smt2@anchor": len(s["wrong_anchor"])}
    for k in [HEAD] + COMPARE:
        f, n = s["scores"].get(k, (None, 0))
        sp = "–" if k == HEAD else (f"{_factor(f)} ({n} fixtures)" if f else "–")
        L.append(f"| {_name(rec, k)} | {s['solved'].get(k, '–')} | {_ms(s['floor'].get(k))} | {sp} | "
                 f"{wrong.get(k, '–')} |")
    L += ["",
          "Speed-up is a weighted geometric mean; above 1× means dv-solve solved "
          "faster. It compares time spent "
          "beyond each solver's own start-up cost, over the fixtures that both "
          "solvers answered and that take at least a millisecond beyond start-up; "
          "most fixtures in this suite are quicker than that, which is why "
          "start-up cost is shown separately. Every dv-solve `sat` or `unsat` is "
          "checked against the reference solvers in the same run, and a wrong "
          "answer is one they contradict; those of the old release are bugs "
          "fixed since. "
          f"dv-solve answers `unknown` on {s['n'] - s['solved'].get(HEAD, 0)} "
          "fixtures that use constructs it does not yet decide "
          "({doc}`../concepts/soundness`).", "",
          "Details per category and per fixture: {doc}`sat`.", "",
          "```{toctree}", ":hidden:", "", "sat", "methodology", "```", ""]
    return "\n".join(L)


def page_sat(rec: dict, s: dict, gen: Path) -> str:
    man = rec["manifests"][SUITE]
    rows = [r for r in rec["sat"] if r["suite"] == SUITE]
    keys = [k for k in ARMS if any(f"{r['arm']}@{r['build']}" == k for r in rows)]
    by = {(r["fixture"], f"{r['arm']}@{r['build']}"): r for r in rows}
    cons = normalize.consensus(rows, SUITE)
    ok = lambda r: normalize._correct(r, cons)

    # Cactus: definite answers only, raw CPU time (start-up included, as a user sees it).
    series = {}
    for k in keys:
        ts = sorted(r["cpu_ms_min"] for r in rows if f"{r['arm']}@{r['build']}" == k and ok(r))
        series[_name(rec, k)] = ts
    (gen / "sat-cactus.svg").write_text(svg.cactus(series, "CPU time per fixture, ms (log scale)",
                                                   "fixtures answered"))
    rat_b = normalize.ratios(rec, SUITE, "bitwuzla@ref")
    groups = {}
    for r, fx in zip(rat_b, man["fixtures"]):
        groups.setdefault(fx["cat"], [])
        if r is not None:
            groups[fx["cat"]].append(math.exp(r / 1000))
    (gen / "sat-vs-bitwuzla.svg").write_text(
        svg.strip({c: v for c, v in groups.items() if v},
                  "dv-solve speed-up (log scale)", _name(rec, "bitwuzla@ref")))

    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(["fixture", "category", "arm", "verdict", "cpu_ms_min", "cpu_ms_med", "wall_ms_min", "reps"])
    for r in rows:
        w.writerow([r["fixture"], r["cat"], _name(rec, f"{r['arm']}@{r['build']}"), r["verdict"],
                    r["cpu_ms_min"], r["cpu_ms_med"], r["wall_ms_min"], r["reps"]])
    (gen / "sat-core.csv").write_text(buf.getvalue())

    L = ["# SMT-LIB2 solving", "",
         f"Each of the {s['n']} fixtures of the `{SUITE}` suite is solved by every "
         "solver several times in alternation, and the minimum CPU time is kept. "
         "boolector runs only on the QF_BV fixtures, since it cannot read the "
         "others' dialect.", "",
         provenance(rec), "",
         "## Fixtures answered within a time", "",
         "Each line counts the fixtures a solver answered correctly within "
         "the CPU time on the x axis, start-up included; further right is slower, "
         "higher is more answered.", "",
         "![Fixtures answered against CPU time](_gen/sat-cactus.svg)", "",
         f"## Speed-up against {_name(rec, 'bitwuzla@ref')}", "",
         "One dot per fixture that both solvers answered and that takes at least a "
         "millisecond beyond start-up; above the dashed line dv-solve is faster.", "",
         "![dv-solve speed-up against bitwuzla, by category](_gen/sat-vs-bitwuzla.svg)", "",
         "## By category", ""]
    cats = list(dict.fromkeys(f["cat"] for f in man["fixtures"]))
    short = {HEAD: "dv-solve", "bitwuzla@ref": "bitwuzla", "z3@ref": "z3",
             "dv-smt2@anchor": (rec.get("anchor") or {}).get("tag", "anchor")}
    for c in cats:
        L.append(f"- **{c}** (weight {man['weights'].get(c, 1.0):g}): {man.get('about', {}).get(c, '')}")
    L += ["", "Fixtures answered correctly (`sat` or `unsat`, agreeing with the references):", "",
          "| Category | Fixtures | " + " | ".join(short[k] for k in [HEAD] + COMPARE) + " |",
          "|---|---|" + "---|" * (1 + len(COMPARE))]
    for c in cats:
        fxs = [f for f in man["fixtures"] if f["cat"] == c]
        ans = [str(sum(1 for f in fxs if ok(by.get((f["path"], k))))) for k in [HEAD] + COMPARE]
        L.append(f"| {c} | {len(fxs)} | {' | '.join(ans)} |")
    rats = {k: normalize.ratios(rec, SUITE, k) for k in COMPARE}
    L += ["", "dv-solve speed-up against each solver, as a geometric mean over the "
          "fixtures counted in brackets:", "",
          "| Category | " + " | ".join(short[k] for k in COMPARE) + " |",
          "|---|" + "---|" * len(COMPARE)]
    for c in cats:
        sc = []
        for k in COMPARE:
            f, n = _cat_score(rats[k], man["fixtures"], c)
            sc.append(f"{_factor(f)} ({n})" if f else "–")
        L.append(f"| {c} | {' | '.join(sc)} |")
    L += ["",
          "## Start-up cost", "",
          "CPU time to answer a one-variable problem: the cost of starting the "
          "process and reading the input, paid once per call.", "",
          "| Solver | CPU ms |", "|---|---|"]
    for k in keys:
        L.append(f"| {_name(rec, k)} | {_ms(s['floor'].get(k))} |")

    L += ["", "## Disagreements", ""]
    if not (s["wrong_head"] or s["wrong_anchor"] or s["ref_split"]):
        L.append("None: every definite answer agrees with the reference solvers.")
    else:
        if not s["wrong_head"]:
            L.append("The current dv-solve agrees with the reference solvers on every fixture.")
        for title, items in (("Current dv-solve", s["wrong_head"]),
                             (f"dv-solve {(rec.get('anchor') or {}).get('tag', '')} "
                              "(old release; these are bugs fixed since)", s["wrong_anchor"]),
                             ("Reference solvers disagreeing with each other", s["ref_split"])):
            if items:
                L += ["", f"**{title}**", ""]
                for d in items:
                    ans = ", ".join(f"{_name(rec, k) if '@' in k else k}: {v}" for k, v in d["answers"].items())
                    L.append(f"- `{Path(d['fixture']).name}`: {ans}")
    unknown = [f for f in man["fixtures"] if (by.get((f["path"], HEAD)) or {}).get("verdict") == "unknown"]
    L += ["", "## Fixtures dv-solve answers `unknown`", "",
          f"{len(unknown)} fixtures use constructs dv-solve does not yet decide, so it "
          "answers `unknown` rather than guess ({doc}`../concepts/soundness`):", ""]
    L.append(", ".join(f"`{Path(f['path']).stem}`" for f in unknown) + "." if unknown else "None.")

    L += ["", "## Every fixture", "",
          "CPU ms, minimum over the repetitions; `–` is no definite answer "
          "(`unknown`, timeout or error); `wrong` is an answer the reference "
          "solvers contradict. "
          "Download: {download}`all measurements as CSV <_gen/sat-core.csv>`.", "",
          "| Fixture | Category | Answer | " + " | ".join(_name(rec, k) for k in keys) + " |",
          "|---|---|---|" + "---|" * len(keys)]
    for f in man["fixtures"]:
        h = by.get((f["path"], HEAD)) or {}
        cells = []
        for k in keys:
            r = by.get((f["path"], k))
            cells.append(_ms(r["cpu_ms_min"]) if ok(r) else ("wrong" if r and r["verdict"] in ("sat", "unsat") else "–"))
        L.append(f"| `{Path(f['path']).stem}` | {f['cat']} | {h.get('verdict', '–')} | {' | '.join(cells)} |")
    L.append("")
    return "\n".join(L)


def render(records_dir, out: Path) -> str:
    gen = out / "_gen"
    gen.mkdir(parents=True, exist_ok=True)
    rec = current(load_records(records_dir))
    if rec is None:
        (out / "index.md").write_text(PLACEHOLDER.format(title="Results") +
                                      "\n```{toctree}\n:hidden:\n\nsat\nmethodology\n```\n")
        (out / "sat.md").write_text(PLACEHOLDER.format(title="SMT-LIB2 solving"))
        return "placeholder"
    s = summary(rec)
    (out / "index.md").write_text(page_index(rec, s))
    (out / "sat.md").write_text(page_sat(rec, s, gen))
    return rec["run"]["utc"]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--records", help="directory of fetched run records (*.json.gz)")
    ap.add_argument("--out", default=str(_REPO / "docs/site/results"))
    ap.add_argument("--max-age-days", type=float, default=None,
                    help="fail if the newest SAT data is older than this")
    a = ap.parse_args(argv)
    what = render(a.records, Path(a.out))
    print(f"rendered results pages from {what}")
    if a.max_age_days is not None:
        newest = max([l["utc"] for l in history.committed_lines() if l.get("sat")]
                     + ([what] if what != "placeholder" else []), default=None)
        if newest is None:
            print("no SAT run recorded yet: placeholder pages")
        elif history._utc_age_days(newest) > a.max_age_days:
            print(f"::error::newest SAT results are from {newest}, older than "
                  f"{a.max_age_days:g} days: is the nightly perf job running?")
            return 1
        elif what == "placeholder":
            print("::error::SAT results are committed but no full run record could "
                  "be fetched to render them")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
