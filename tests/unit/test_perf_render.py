"""Perf ratios, suite scores and the Results pages, on a synthetic run record.

The published speed-ups come from normalize.py; these pin down its exclusion
rules (no definite answer, under the start-up floor) and the category cap,
and check that render.py produces every page from a record.
"""
from __future__ import annotations

import math

from tests.perf import normalize, render, schema

FX = [{"path": f"tests/formal/smt2/{c}/f{i}.smt2", "sha": "0", "cat": c}
      for c, n in (("tier1", 3), ("verilator", 2)) for i in range(n)]


def _row(fx, arm, build, verdict, ms):
    return {"suite": "sat-core", "fixture": fx["path"], "sha": "0", "cat": fx["cat"],
            "arm": arm, "build": build, "verdict": verdict, "cpu_ms_min": ms,
            "cpu_ms_med": ms, "wall_ms_min": ms, "reps": 5, "rss_kb": 1}


def _record():
    rows = []
    head = [11.0, 3.0, 1.2, 21.0, 5.0]           # f2 is under the floor margin
    bwz = [21.0, 5.0, 1.4, None, 9.0]            # verilator/f0: bitwuzla unknown
    for fx, h, b in zip(FX, head, bwz):
        rows.append(_row(fx, "dv-smt2", "head", "sat", h))
        rows.append(_row(fx, "bitwuzla", "ref", "sat" if b else "unknown", b))
        rows.append(_row(fx, "z3", "ref", "sat", 2 * h))
        rows.append(_row(fx, "dv-smt2", "anchor", "sat", h))
    return {"schema": 1, "valid": True, "kind": "nightly",
            "run": {"utc": "20261003T050000Z", "commit": "a" * 40, "ref": "refs/heads/main"},
            "machine": {"id": "m0", "cpu": "cpu", "cores": 8, "mem_gb": 16, "kernel": "6.8",
                        "loadavg_start": 0.1, "loadavg_end": 0.1, "noisy": False},
            "tools": {"lock_sha": "x", "z3": "5.1.0", "bitwuzla": "0.8.2"},
            "anchor": {"tag": "v0.1.0", "commit": "b" * 40}, "calib": {"kernel_cpu_ms": {}},
            "manifests": {"sat-core": {"hash": "h", "suite": "sat-core", "fixtures": FX,
                                       "weights": {"tier1": 1.0, "verilator": 2.0},
                                       "about": {"tier1": "t", "verilator": "v"}}},
            "budgets": {"sat-core": 10}, "sat": rows,
            "sat_floor": {"sat-core": {"dv-smt2@head": 1.0, "bitwuzla@ref": 1.0,
                                       "z3@ref": 1.0, "dv-smt2@anchor": 1.0}},
            "sat_disagree": {"sat-core": []}}


def test_ratios_subtract_startup_and_exclude():
    r = normalize.ratios(_record(), "sat-core", "bitwuzla@ref")
    assert r[0] == round(1000 * math.log(20 / 10))   # (21-1)/(11-1): 2x faster
    assert r[2] is None                              # both within 1 ms of start-up
    assert r[3] is None                              # bitwuzla had no definite answer
    assert r[4] == round(1000 * math.log(8 / 4))


def test_score_is_category_weighted_and_capped():
    rec = _record()
    man = rec["manifests"]["sat-core"]
    z3 = normalize.ratios(rec, "sat-core", "z3@ref")
    assert all(x is None or x > 0 for x in z3)
    # identical builds: the anchor speed-up is exactly 1
    assert abs(normalize.score(normalize.ratios(rec, "sat-core", "dv-smt2@anchor"),
                               man["fixtures"], man["weights"]) - 1) < 1e-9
    share = normalize._capped({"a": 10, "b": 1, "c": 1})
    assert abs(share["a"] - normalize.CAT_CAP) < 1e-9 and abs(sum(share.values()) - 1) < 1e-9


def test_trend_line_carries_ratios_in_manifest_order():
    line = schema.trend_lines(_record(), "perf-nightly-20261003T050000Z-aaaaaaa")[0]
    sat = line["sat"]["sat-core"]
    assert sat["m"] == "h" and len(sat["vs"]["bitwuzla@ref"]) == len(FX)
    assert sat["solved"]["dv-smt2@head"] == 5 and sat["solved"]["bitwuzla@ref"] == 4


def test_render_writes_every_page(tmp_path, monkeypatch):
    monkeypatch.setattr(render, "load_records", lambda d: [_record()])
    assert render.render(None, tmp_path) == "20261003T050000Z"
    index, sat = (tmp_path / "index.md").read_text(encoding="utf-8"), (tmp_path / "sat.md").read_text(encoding="utf-8")
    assert "bitwuzla 0.8.2" in index and "v0.1.0" in index
    assert "## Every fixture" in sat and "f0" in sat
    for f in ("sat-cactus.svg", "sat-vs-bitwuzla.svg", "sat-core.csv"):
        assert (tmp_path / "_gen" / f).stat().st_size > 0


def test_render_placeholder_without_data(tmp_path, monkeypatch):
    monkeypatch.setattr(render, "load_records", lambda d: [])
    assert render.render(None, tmp_path) == "placeholder"
    assert "toctree" in (tmp_path / "index.md").read_text(encoding="utf-8")


def test_wrong_answer_is_not_a_fast_answer():
    """v0.1.0 answers t_constraint_operators `unsat` in 0.3 ms; the answer is sat."""
    rec = _record()
    for r in rec["sat"]:
        if r["build"] == "anchor" and r["fixture"] == FX[0]["path"]:
            r["verdict"], r["cpu_ms_min"] = "unsat", 1.3
    assert normalize.ratios(rec, "sat-core", "dv-smt2@anchor")[0] is None
    assert normalize.solved(rec, "sat-core")["dv-smt2@anchor"] == 4


def _rand_rows():
    def row(bench, arm, build, jsd, cpu, cov=1.0, bad=0):
        r = {"bench": bench, "arm": arm, "build": build, "n": 100, "cov": cov, "jsd": jsd,
             "chi2p": 0.5, "distinct": 0.5, "off": 0, "bad": bad, "hist": [1.0, 1.0]}
        if arm != "uniform":
            r.update(cpu_ms=cpu, p50_ms=cpu, p95_ms=cpu, p99_ms=cpu, checks=5.0)
        return r
    rows = []
    for b in ("countones", "packet"):
        rows += [row(b, "uniform", "ref", 0.10, None),
                 row(b, "dv-api@head", "head", 0.11, 0.002),
                 row(b, "dv-swizzle@head", "head", 0.12, 0.5),
                 row(b, "z3-swizzle", "ref", 0.30, 5.0, cov=0.7),
                 row(b, "bitwuzla-swizzle", "ref", 0.05, 20.0)]   # below the floor
    rows.append({"bench": "packet", "arm": "dv-swizzle@anchor", "build": "anchor",
                 "error": "randomize() took over 10 s"})
    return rows


def _rand_record():
    rec = _record()
    rec["manifests"]["rand-core"] = {
        "hash": "r", "suite": "rand-core", "kind": "rand", "protocol": "verilator-5.046",
        "benches": [{"name": "countones", "n": 100, "sha": "0", "about": "c", "space": 70,
                     "bins": None},
                    {"name": "packet", "n": 100, "sha": "1", "about": "p", "space": None,
                     "bins": 144}]}
    rec["rand"] = _rand_rows()
    return rec


def test_rand_summary_excess_jsd_and_speedup():
    rs = render.rand_summary(_rand_record())
    assert abs(rs["arm"]["z3-swizzle"]["xjsd"] - 0.20) < 1e-9
    assert rs["arm"]["bitwuzla-swizzle"]["xjsd"] == 0.0      # under the floor is noise, not 0-
    assert abs(rs["speedup"] - 10.0) < 1e-9                  # 5 ms against 0.5 ms
    assert rs["arm"]["dv-swizzle@anchor"]["errors"] == 1


def test_rand_trend_line():
    line = schema.trend_lines(_rand_record(), "perf-nightly-20261003T050000Z-aaaaaaa")[0]
    r = line["rand"]["rand-core"]
    assert r["m"] == "r" and r["protocol"] == "verilator-5.046" and r["errors"] == 1
    assert r["b"]["countones"]["z3-swizzle"] == [5.0, 0.2, 0.7]
    assert "uniform" not in r["b"]["countones"]


def test_render_randomization_page(tmp_path, monkeypatch):
    monkeypatch.setattr(render, "load_records", lambda d: [_rand_record()])
    render.render(None, tmp_path)
    page = (tmp_path / "randomization.md").read_text(encoding="utf-8")
    index = (tmp_path / "index.md").read_text(encoding="utf-8")
    assert "Verilator + z3 5.1.0" in page and "failed: randomize()" in page
    assert "dv-solve is 10" in page and "faster per randomize()" in page
    assert "## Randomization at a glance" in index and "randomization" in index
    for f in ("rand-quality-cost.svg", "rand-histograms.svg", "rand-cost.svg", "rand-core.csv"):
        assert (tmp_path / "_gen" / f).stat().st_size > 0
