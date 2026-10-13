"""Pinned solver binaries for perf runs (design §5.3).

    python3 -m tests.perf.tools fetch --dest DIR   # download, verify sha256, unpack

Tarballs are pinned by URL and sha256 in tools.lock.json; pip tools (z3) in
solvers.lock. `path(name)` looks in $DVS_PERF_TOOLS (where `fetch` unpacked;
default perf-builds/tools), then in the ivpm-managed packages/ directory of a
developer checkout, and
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
import zipfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
LOCK = json.loads((_HERE / "tools.lock.json").read_text(encoding="utf-8"))
# Developer checkouts get these from ivpm (ivpm.yaml); same bundles, older layout.
_LOCAL = {"bitwuzla": _REPO / "packages/verilator-bin/bin/bitwuzla",
          "verilator": _REPO / "packages/verilator-bin/bin/verilator",
          "boolector": _REPO / "packages/yosys-bin/bin/boolector",
          "yosys": _REPO / "packages/yosys/bin/yosys",
          "yosys-abc": _REPO / "packages/yosys/bin/yosys-abc"}
DEFAULT_ROOT = _REPO / "perf-builds" / "tools"
# Every pinned SMT solver, in the order reports list them.
SOLVERS = ("z3", "bitwuzla", "boolector", "cvc5", "yices")


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
            root = Path(os.environ.get("DVS_PERF_TOOLS") or DEFAULT_ROOT)
            if (root / rel).exists():
                return root / rel
    if name == "z3":
        z = shutil.which("z3")
        return Path(z) if z else None
    p = _LOCAL.get(name)
    return p if p and p.exists() else None


def version(name: str):
    p = path(name)
    if p is None:
        return None
    # yosys-abc has no --version; `-q version` prints "UC Berkeley, ABC 1.01 (...)"
    argv = [str(p), "-q", "version"] if name == "yosys-abc" else [str(p), "--version"]
    try:
        out = subprocess.run(argv, capture_output=True, text=True, timeout=30).stdout.split()
    except (OSError, subprocess.TimeoutExpired):
        return None
    # word holding the version: "Z3 version 5.1.0 - 64 bit", "Verilator 5.046 ...",
    # "cvc5 1.4.1 [git ...]", "Yices 2.6.5", "Yosys 0.69 (git ...)"; bitwuzla and
    # boolector print the bare version
    at = {"z3": 2, "yosys-abc": 3}.get(name, 1 if name in (
        "verilator", "cvc5", "yices", "yosys") else 0)
    return out[at] if len(out) > at else None


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
        arc = dest / f"{pkg}.download"
        with urllib.request.urlopen(spec["url"], timeout=300) as r, open(arc, "wb") as f:
            shutil.copyfileobj(r, f)
        digest = hashlib.sha256(arc.read_bytes()).hexdigest()
        if digest != spec["sha256"]:
            arc.unlink()
            raise RuntimeError(f"{pkg}: sha256 {digest} does not match the lock")
        if zipfile.is_zipfile(arc):          # cvc5's release, yices' wheel
            with zipfile.ZipFile(arc) as z:
                z.extractall(dest)
            for rel, _ in spec["provides"].values():    # zip drops the mode bits
                (dest / rel).chmod(0o755)
        else:
            with tarfile.open(arc) as t:
                t.extractall(dest)
        arc.unlink()


# Default reference solvers for a SAT suite with no "references" of its own
# (run_sat.REFERENCES, duplicated here to avoid importing run_sat, which
# imports this module).
_DEFAULT_REFS = ("z3", "bitwuzla", "boolector")


def needed_for(suite_names) -> set:
    """Tool names actually exercised by `--suites suite_names` (design §2.2).

    Only a suite's own declared `references` (or the SAT default) are
    required; a suite with no SMT2 fixtures (calib, rand, scenario) needs
    only z3. Tools outside this set, such as yosys/yosys-abc, are used by
    one-off scripts that call tools.path() directly and fail on their own
    if the tool is missing -- they should not block every other suite.
    """
    names = {"z3"}
    for name in suite_names:
        spec_path = _HERE / "suites" / f"{name}.json"
        if not spec_path.exists():
            continue
        spec = json.loads(spec_path.read_text(encoding="utf-8"))
        if spec.get("kind") in ("rand", "scenario"):
            continue
        names.update(spec.get("references", _DEFAULT_REFS))
    return names


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="pinned perf tools")
    sub = ap.add_subparsers(dest="cmd", required=True)
    fetch_p = sub.add_parser("fetch")
    fetch_p.add_argument("--dest", default=str(DEFAULT_ROOT))
    fetch_p.add_argument("--suites", default=None,
                          help="comma-separated suite names: check only the tools "
                               "they use, instead of every pinned tool")
    sub.add_parser("check")
    a = ap.parse_args(argv)
    if a.cmd == "fetch":
        fetch(Path(a.dest))
        os.environ["DVS_PERF_TOOLS"] = a.dest
    want = needed_for(a.suites.split(",")) if getattr(a, "suites", None) else \
        {*SOLVERS, "yosys", "yosys-abc"}
    print(json.dumps(check(sorted(want))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
