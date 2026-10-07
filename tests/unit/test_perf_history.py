"""Perf history plumbing: record schema, trend lines, consolidation.

Consolidation is the only way run records become committed history, and an
artifact it mishandles is lost when the artifact expires, so it must be
idempotent, order-independent and must not drop invalid runs.
"""
from __future__ import annotations

import gzip
import json

from tests.perf import consolidate, history, schema


def _rec(utc, commit="a" * 40, kind="nightly", valid=True, ref="refs/heads/main", noisy=False):
    r = {"schema": 1, "valid": valid, "kind": kind,
         "run": {"utc": utc, "commit": commit, "ref": ref, "dv_version": None, "workflow_run": 1},
         "machine": {"id": "m00000", "cpu": "cpu", "cores": 8, "mem_gb": 16, "kernel": "6.8",
                     "loadavg_start": 0.1, "loadavg_end": 0.2, "noisy": noisy},
         "tools": {"lock_sha": "x", "z3": "5.1.0"}, "anchor": None,
         "calib": {"kernel_cpu_ms": {"z3-ops": 2000.0}, "reps": 5}}
    if not valid:
        r["reason"] = "kernel answered wrongly"
    return r


def _art(rec):
    return schema.artifact_name(rec["kind"], rec["run"]["utc"], rec["run"]["commit"])


def test_validate_catches_missing_and_bad_fields():
    assert schema.validate(_rec("20261003T050000Z")) == []
    bad = _rec("2026-10-03")
    del bad["machine"]["noisy"]
    errs = schema.validate(bad)
    assert any("utc" in e for e in errs) and any("noisy" in e for e in errs)
    inv = _rec("20261003T050000Z", valid=False)
    del inv["reason"]
    assert schema.validate(inv) == ["invalid record without a reason"]


def test_artifact_name_round_trips():
    n = _art(_rec("20261003T050000Z"))
    m = schema.ARTIFACT_RE.match(n)
    assert m and m["kind"] == "nightly" and m["utc"] == "20261003T050000Z"


def test_consolidate_is_idempotent_and_sorted(tmp_path):
    recs = [_rec("20261004T050000Z", commit="b" * 40), _rec("20261003T050000Z")]
    recs = [(_art(r), r) for r in recs]
    s1 = consolidate.consolidate(recs, tmp_path)
    text = (tmp_path / "2026.jsonl").read_text(encoding="utf-8")
    s2 = consolidate.consolidate(list(reversed(recs)), tmp_path)
    assert s1["lines"] == 2 and s2["lines"] == 0
    assert (tmp_path / "2026.jsonl").read_text(encoding="utf-8") == text
    utcs = [json.loads(l)["utc"] for l in text.splitlines()]
    assert utcs == sorted(utcs)
    assert (tmp_path / "machines" / "m00000.json").exists()


def test_invalid_and_noisy_runs_are_kept_with_reason(tmp_path):
    bad = _rec("20261003T050000Z", valid=False, noisy=True)
    s = consolidate.consolidate([(_art(bad), bad)], tmp_path)
    line = json.loads((tmp_path / "2026.jsonl").read_text(encoding="utf-8"))
    assert s["invalid"] == 1 and s["noisy"] == 1
    assert line["valid"] is False and line["noisy"] is True and line["reason"]


def test_malformed_record_is_rejected_not_written(tmp_path):
    bad = _rec("20261003T050000Z")
    bad["schema"] = 99
    s = consolidate.consolidate([("perf-nightly-20261003T050000Z-aaaaaaa", bad)], tmp_path)
    assert s["rejected"] and not (tmp_path / "2026.jsonl").exists()


def test_release_snapshot_earliest_valid_wins(tmp_path):
    first = _rec("20261010T140000Z", kind="release", ref="refs/tags/v0.2.0")
    later = _rec("20261012T140000Z", kind="release", ref="refs/tags/v0.2.0", commit="c" * 40)
    consolidate.consolidate([(_art(first), first)], tmp_path)
    s = consolidate.consolidate([(_art(later), later)], tmp_path)
    snap = json.loads(gzip.decompress((tmp_path / "releases" / "v0.2.0.json.gz").read_bytes()))
    assert snap["run"]["utc"] == "20261010T140000Z" and s["releases"] == []


def test_unconsolidated_is_what_is_newer_than_history():
    lines = [{"utc": "20261003T050000Z"}]
    arts = [("perf-nightly-20261002T050000Z-aaaaaaa", 1),
            ("perf-nightly-20261003T050000Z-aaaaaaa", 2),
            ("perf-nightly-20261004T050000Z-bbbbbbb", 3)]
    assert history.unconsolidated(arts, lines) == [arts[2]]
