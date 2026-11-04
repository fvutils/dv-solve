"""Pinned tool locks: the solver lock and the simulator lock stay separate.

A simulator pin moving must not change the lock hash of the solver perf
records (it would mark a false break in their trend), and two bundles that
unpack to the same directory (both Verilator bundles unpack to verilator/)
must not overwrite each other.
"""
from __future__ import annotations

from tests.perf import tools


def test_tool_names_unique_across_locks():
    names = [n for n, _, _ in tools._pinned()]
    assert len(names) == len(set(names))


def test_sim_lock_has_its_own_hash():
    assert tools.lock_sha("perf") != tools.lock_sha("sim")
    assert {n for n, _, _ in tools._pinned("perf")}.isdisjoint(
        {n for n, _, _ in tools._pinned("sim")})


def test_sim_bundles_unpack_apart():
    for pkg, spec in tools._packages("sim"):
        into = spec["into"]
        for rel, _ in spec["provides"].values():
            assert rel.startswith(into + "/"), (pkg, rel)
        for key in ("manifest", "build_info"):
            if key in spec:
                assert spec[key].startswith(into + "/"), (pkg, key)


def test_provenance_unfetched_tool_has_lock_identity(tmp_path, monkeypatch):
    monkeypatch.setenv("DVS_PERF_TOOLS", str(tmp_path))
    p = tools.provenance("xezim")
    assert p["package"].startswith("xezim-bin-")
    assert len(p["sha256"]) == 64
    assert "release" not in p          # no manifest: nothing fetched
    assert tools.provenance("no-such-tool") == {}
