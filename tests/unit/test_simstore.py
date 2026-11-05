"""bench-data: records in, trend lines out, idempotent (plan §6a)."""
from __future__ import annotations

import json

from tests.perf import simstore


def _rec(utc, kind="nightly", xz_out=("ok", "failed"), verilator="5.054", ses="abc"):
    def rows(outs, us, sess=None):
        return [{"bench": b, "outcome": o, "us": u, "spread": {"min_entropy": 0.5, "stuck": 0},
                 **({"ses": sess} if sess else {})}
                for b, o, u in zip(("a.x", "b.y"), outs, us)]
    return {"valid": True, "kind": kind, "run": {"utc": utc, "commit": "c" * 40, "workflow_run": "7"},
            "tools": {"sims": {"verilator-sc": {"src": "latest"}, "xezim": {"src": "lock"}}},
            "sim": {"manifest": "m1", "benches": [["a.x", "1"], ["b.y", "2"]], "noisy": False,
                    "variants": {
                        "dvs-standalone": {"sim": "verilator@" + verilator, "dvs": {"commit": "d1"},
                                           "geo_us": 2.0, "counts": {"ok": 2},
                                           "benches": rows(("ok", "ok"), (1.0, 4.0), ses)},
                        "xezim": {"sim": "xezim@0.11.3+a.b", "dvs": None,
                                  "benches": rows(xz_out, (3.0, 9.0))},
                        "vlt-dvs-lib": {"skipped": "no simulator support"}}}}


def test_trend_lines():
    ln = {l["variant"]: l for l in simstore.trend_lines(_rec("20261105T053000Z"), "n")}
    assert set(ln) == {"dvs-standalone", "xezim"}          # skipped variants have no line
    assert ln["xezim"]["out"] == "of" and ln["xezim"]["us"] == [3.0, None]
    assert ln["xezim"]["src"] == "lock" and ln["dvs-standalone"]["src"] == "latest"
    assert ln["dvs-standalone"]["ses"] == ["abc", "abc"] and ln["xezim"]["ses"] == [None, None]
    assert ln["dvs-standalone"]["dvs"] == "d1" and ln["xezim"]["dvs"] is None


def test_add_is_idempotent(tmp_path):
    a = ("simbench-nightly-20261105T053000Z-ccccccc", _rec("20261105T053000Z"))
    b = ("simbench-reference-20261106T053000Z-ccccccc",
         _rec("20261106T053000Z", "reference", ("ok", "ok")))
    manual = ("simbench-manual-20261106T060000Z-ccccccc", _rec("20261106T060000Z", "manual"))
    assert simstore.add(tmp_path, [a, manual]) == [a[0]]           # manual runs are not kept
    snap = {p: p.read_bytes() for p in tmp_path.rglob("*") if p.is_file()}
    assert simstore.add(tmp_path, [a]) == []
    assert {p: p.read_bytes() for p in tmp_path.rglob("*") if p.is_file()} == snap
    assert simstore.add(tmp_path, [b, a]) == [b[0]]
    lines = [json.loads(l) for l in (tmp_path / "history/2026.jsonl").read_text().splitlines()]
    assert [l["out"] for l in lines if l["variant"] == "xezim"] == ["of", "oo"]   # a fix shows
    sims = json.loads((tmp_path / "sims.json").read_text())
    assert sims["xezim"]["xezim@0.11.3+a.b"]["runs"] == 2
    assert json.loads((tmp_path / "manifests/m1.json").read_text()) == [["a.x", "1"], ["b.y", "2"]]


def test_old_records_get_a_manifest_from_their_rows():
    r = _rec("20261104T232707Z")
    del r["sim"]["benches"]
    assert simstore.manifest_of(r) == [["a.x", None], ["b.y", None]]


def test_choose_kind():
    have = {"vlt-z3-default": {"verilator@5.054": {}}}
    assert simstore.choose_kind("schedule", "", 3, "5.054", have) == "nightly"
    assert simstore.choose_kind("schedule", "", 6, "5.054", have) == "reference"     # Sunday
    assert simstore.choose_kind("schedule", "", 3, "5.060", have) == "reference"     # new Verilator
    assert simstore.choose_kind("schedule", "", 3, "5.054", {}) == "reference"       # first night
    assert simstore.choose_kind("workflow_dispatch", "manual", 6, "5.060", {}) == "manual"
