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


def _feed(tmp_path, version="5.060", digest=None, tag=None):
    """A fake GitHub: releases/latest, the asset and its .sha256."""
    import hashlib, io, json, tarfile
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as t:
        data = b"#!/bin/sh\n"
        ti = tarfile.TarInfo("verilator/bin/verilator")
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
    blob = buf.getvalue()
    name = f"verilator-manylinux_2_28_x86_64-{version}.tar.gz"
    files = {"latest": json.dumps({"tag_name": tag or "v" + version, "assets": [
                {"name": name, "browser_download_url": "A"},
                {"name": name + ".sha256", "browser_download_url": "S"}]}).encode(),
             "A": blob,
             "S": ((digest or hashlib.sha256(blob).hexdigest()) + "  " + name + "\n").encode()}
    return lambda url, timeout=60: files["latest" if url.endswith("/latest") else url]


def test_latest_fetches_the_newest_release(tmp_path):
    got = tools.latest(tmp_path, "sim", get=_feed(tmp_path))
    v = got["verilator-sc"]
    assert v["src"] == "latest" and v["version"] == "5.060"
    assert v["path"].endswith("verilator-bin@5.060/verilator/bin/verilator")


def test_latest_falls_back_to_the_pin(tmp_path):
    bad = tools.latest(tmp_path, "sim", get=_feed(tmp_path, digest="0" * 64))
    assert bad["verilator-sc"]["src"] == "lock" and "sha256" in bad["verilator-sc"]["error"]
    assert bad["verilator-sc"]["version"] == "5.054"
    old = tools.latest(tmp_path, "sim", get=_feed(tmp_path, version="5.052"))
    assert old["verilator-sc"]["src"] == "lock" and "older than" in old["verilator-sc"]["error"]
