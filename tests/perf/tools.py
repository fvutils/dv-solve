"""Pinned solver binaries for perf runs (design §5.3).

    python3 -m tests.perf.tools fetch --dest DIR   # download, verify sha256, unpack

Tarballs are pinned by URL and sha256 in tools.lock.json; pip tools (z3) in
solvers.lock. `path(name)` looks in $DVS_PERF_TOOLS (where `fetch` unpacked)
and then in the ivpm-managed packages/ directory of a developer checkout, and
`check()` refuses a binary whose version is not the pinned one, so a run can
never silently measure a different solver.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
LOCK = json.loads((_HERE / "tools.lock.json").read_text())
# Developer checkouts get these from ivpm (ivpm.yaml); same bundles, older layout.
_LOCAL = {"bitwuzla": _REPO / "packages/verilator-bin/bin/bitwuzla",
          "verilator": _REPO / "packages/verilator-bin/bin/verilator",
          "boolector": _REPO / "packages/yosys-bin/bin/boolector"}


def lock_sha() -> str:
    h = hashlib.sha256()
    for f in ("solvers.lock", "tools.lock.json"):
        h.update((_HERE / f).read_bytes())
    return h.hexdigest()[:12]


def _pinned():
    for pkg in LOCK.values():
        if isinstance(pkg, dict):
            for name, (rel, ver) in pkg["provides"].items():
                yield name, rel, ver


def path(name: str):
    for n, rel, _ in _pinned():
        if n == name:
            root = os.environ.get("DVS_PERF_TOOLS")
            if root and (Path(root) / rel).exists():
                return Path(root) / rel
    if name == "z3":
        z = shutil.which("z3")
        return Path(z) if z else None
    p = _LOCAL.get(name)
    return p if p and p.exists() else None


def version(name: str):
    p = path(name)
    if p is None:
        return None
    try:
        out = subprocess.run([str(p), "--version"], capture_output=True, text=True,
                             timeout=30).stdout.split()
    except (OSError, subprocess.TimeoutExpired):
        return None
    if name in ("z3", "verilator"):          # "Z3 version 5.1.0 - 64 bit", "Verilator 5.046 ..."
        return out[2] if name == "z3" and len(out) > 2 else (out[1] if len(out) > 1 else None)
    return out[0] if out else None


def check(names) -> dict:
    """{name: version}; raises if a pinned tool is missing or the wrong version."""
    want = {n: v for n, _, v in _pinned()}
    got = {}
    for n in names:
        v = version(n)
        if v is None:
            raise RuntimeError(f"{n}: not found (run `python3 -m tests.perf.tools fetch`)")
        if n in want and v != want[n]:
            raise RuntimeError(f"{n}: version {v}, pinned {want[n]}")
        got[n] = v
    return got


def fetch(dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for pkg, spec in LOCK.items():
        if not isinstance(spec, dict):
            continue
        if all((dest / rel).exists() for rel, _ in spec["provides"].values()):
            continue
        tgz = dest / f"{pkg}.tar.gz"
        with urllib.request.urlopen(spec["url"], timeout=300) as r, open(tgz, "wb") as f:
            shutil.copyfileobj(r, f)
        digest = hashlib.sha256(tgz.read_bytes()).hexdigest()
        if digest != spec["sha256"]:
            tgz.unlink()
            raise RuntimeError(f"{pkg}: sha256 {digest} does not match the lock")
        with tarfile.open(tgz) as t:
            t.extractall(dest)
        tgz.unlink()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="pinned perf tools")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("fetch").add_argument("--dest", required=True)
    sub.add_parser("check")
    a = ap.parse_args(argv)
    if a.cmd == "fetch":
        fetch(Path(a.dest))
        os.environ["DVS_PERF_TOOLS"] = a.dest
    print(json.dumps(check(["z3", "bitwuzla", "boolector"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
