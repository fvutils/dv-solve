"""Pinned solver binaries for perf runs (design §5.3).

    python3 -m tests.perf.tools fetch --dest DIR   # download, verify sha256, unpack
    python3 -m tests.perf.tools latest --lock sim --dest DIR --out FILE
                                                   # the newest release of each
                                                   # bundle with a `latest` feed

Tarballs are pinned by URL and sha256 in tools.lock.json; pip tools (z3) in
solvers.lock. The simulators simbench runs are pinned in sim_tools.lock.json
(`fetch --lock sim`), which has its own lock hash. `path(name)` looks in $DVS_PERF_TOOLS (where `fetch` unpacked;
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
LOCK_FILES = {"perf": "tools.lock.json", "sim": "sim_tools.lock.json"}
LOCKS = {k: json.loads((_HERE / f).read_text(encoding="utf-8")) for k, f in LOCK_FILES.items()}
LOCK = LOCKS["perf"]
# Developer checkouts get these from ivpm (ivpm.yaml); same bundles, older layout.
_LOCAL = {"bitwuzla": _REPO / "packages/verilator/bin/bitwuzla",
          "verilator": _REPO / "packages/verilator/bin/verilator",
          "boolector": _REPO / "packages/yosys-bin/bin/boolector",
          "yosys": _REPO / "packages/yosys/bin/yosys",
          "yosys-abc": _REPO / "packages/yosys/bin/yosys-abc"}
DEFAULT_ROOT = _REPO / "perf-builds" / "tools"
# Every pinned SMT solver, in the order reports list them.
SOLVERS = ("z3", "bitwuzla", "boolector", "cvc5", "yices")


def lock_sha(lock: str = "perf") -> str:
    h = hashlib.sha256()
    for f in ("solvers.lock", LOCK_FILES[lock]):
        h.update((_HERE / f).read_bytes())
    return h.hexdigest()[:12]


def _packages(lock=None):
    for k in ([lock] if lock else LOCKS):
        for pkg, spec in LOCKS[k].items():
            if isinstance(spec, dict):
                yield pkg, spec


def _pinned(lock=None):
    for _, spec in _packages(lock):
        for name, (rel, ver) in spec["provides"].items():
            yield name, rel, ver


def _root() -> Path:
    return Path(os.environ.get("DVS_PERF_TOOLS") or DEFAULT_ROOT)


def path(name: str):
    for n, rel, _ in _pinned():
        if n == name:
            if (_root() / rel).exists():
                return _root() / rel
    if name == "z3":
        z = shutil.which("z3")
        return Path(z) if z else None
    p = _LOCAL.get(name)
    return p if p and p.exists() else None


def version(name: str):
    p = path(name)
    if p is None:
        return None
    # yosys-abc has no --version; `-q version` prints "UC Berkeley, ABC 1.01 (...)";
    # xezim takes -V ("xezim version 0.11.3 / git 058f2bba (...) / tag 0.11.3")
    argv = [str(p)] + {"yosys-abc": ["-q", "version"], "xezim": ["-V"]}.get(name, ["--version"])
    try:
        out = subprocess.run(argv, capture_output=True, text=True, timeout=30).stdout.split()
    except (OSError, subprocess.TimeoutExpired):
        return None
    # word holding the version: "Z3 version 5.1.0 - 64 bit", "Verilator 5.046 ...",
    # "cvc5 1.4.1 [git ...]", "Yices 2.6.5", "Yosys 0.69 (git ...)"; bitwuzla and
    # boolector print the bare version
    at = {"z3": 2, "yosys-abc": 3, "xezim": 2}.get(name, 1 if name in (
        "verilator", "verilator-sc", "cvc5", "yices", "yosys") else 0)
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


def provenance(name: str) -> dict:
    """What a pinned bundle says about its own build: the edapack manifest's
    release (version, track, recipe) and inputs (repo, ref, resolved sha), and
    BUILD-INFO.txt's `name value` lines where the bundle has one (xezim's
    names xezim-core, which the manifest does not). {} for a tool that is not
    a fetched bundle."""
    for pkg, spec in _packages():
        if name not in spec["provides"]:
            continue
        out = {"package": pkg, "sha256": spec["sha256"]}
        m = _root() / spec.get("manifest", "")
        if spec.get("manifest") and m.exists():
            d = json.loads(m.read_text(encoding="utf-8"))
            out["release"] = {k: d.get("release", {}).get(k) for k in (
                "version", "tag", "track", "built_at", "recipe_sha")}
            out["inputs"] = {i["name"]: {"ref": i.get("ref"), "sha": i.get("resolved_sha"),
                                         "version": i.get("version")}
                             for i in d.get("inputs", [])}
        b = _root() / spec.get("build_info", "")
        if spec.get("build_info") and b.exists():
            lines = b.read_text(encoding="utf-8").splitlines()
            out["build_info"] = dict(ln.split(None, 1) for ln in lines[1:] if len(ln.split(None, 1)) == 2)
        return out
    return {}


def fetch(dest: Path, lock: str = "perf") -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for pkg, spec in _packages(lock):
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
                z.extractall(dest / spec.get("into", ""))
            for rel, _ in spec["provides"].values():    # zip drops the mode bits
                (dest / rel).chmod(0o755)
        else:
            with tarfile.open(arc) as t:
                t.extractall(dest / spec.get("into", ""))
        arc.unlink()


def _vkey(v: str) -> tuple:
    return tuple(int(x) if x.isdigit() else 0 for x in v.split("."))


def _get(url: str, timeout: int = 60) -> bytes:
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                               "User-Agent": "dv-solve-perf-tools"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def _resolve(feed: dict, get=_get) -> dict:
    """The newest release on a GitHub release feed: {version, url, sha256}.
    GitHub's releases/latest skips prereleases, which is where edapack's
    snapshot builds go, so this is the newest real release."""
    rel = json.loads(get(f"https://api.github.com/repos/{feed['repo']}/releases/latest"))
    version = rel["tag_name"].lstrip("v")
    if _vkey(version) < _vkey(feed["min"]):
        raise RuntimeError(f"newest release {version} is older than {feed['min']}")
    name = feed["asset"].format(version=version)
    urls = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}
    if name not in urls or name + ".sha256" not in urls:
        raise RuntimeError(f"release {version} has no {name} (or its .sha256)")
    sha = get(urls[name + ".sha256"]).decode().split()[0].lower()
    return {"version": version, "url": urls[name], "sha256": sha}


def latest(dest: Path, lock: str = "sim", get=_get) -> dict:
    """Fetch the newest release of every bundle in `lock` with a `latest` feed
    into dest/<package>@<version>/, sha256-checked against the release's own
    .sha256. Returns {tool name: {path, version, url, sha256, src}}. On any
    failure the tool falls back to its pin (`src: "lock"`, with the error),
    which `fetch` must already have unpacked under $DVS_PERF_TOOLS."""
    dest.mkdir(parents=True, exist_ok=True)
    out = {}
    for pkg, spec in _packages(lock):
        feed = spec.get("latest")
        if not feed:
            continue
        (name, (rel_pin, ver_pin)), = spec["provides"].items()
        try:
            r = _resolve(feed, get)
            into = dest / f"{pkg.rsplit('-', 1)[0]}@{r['version']}"
            tool = into / feed["bin"]
            if not tool.exists():
                arc = dest / f"{pkg}.download"
                arc.write_bytes(get(r["url"], timeout=300))
                digest = hashlib.sha256(arc.read_bytes()).hexdigest()
                if digest != r["sha256"]:
                    arc.unlink()
                    raise RuntimeError(f"sha256 {digest} does not match the release's {r['sha256']}")
                with tarfile.open(arc) as t:
                    t.extractall(into)
                arc.unlink()
            out[name] = dict(r, path=str(tool.resolve()), src="latest")
        except Exception as e:           # noqa: BLE001 -- any failure falls back to the pin
            out[name] = {"path": str((_root() / rel_pin).resolve()), "version": ver_pin,
                         "url": spec["url"], "sha256": spec["sha256"], "src": "lock",
                         "error": f"{type(e).__name__}: {e}"[:300]}
    return out


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
    fetch_p.add_argument("--lock", choices=sorted(LOCK_FILES), default="perf")
    fetch_p.add_argument("--suites", default=None,
                          help="comma-separated suite names: check only the tools "
                               "they use, instead of every pinned tool")
    check_p = sub.add_parser("check")
    check_p.add_argument("--lock", choices=sorted(LOCK_FILES), default="perf")
    latest_p = sub.add_parser("latest", help="fetch the newest release of each floating bundle")
    latest_p.add_argument("--lock", choices=sorted(LOCK_FILES), default="sim")
    latest_p.add_argument("--dest", required=True)
    latest_p.add_argument("--out", required=True, help="JSON: tool -> path, version, sha256, src")
    prov_p = sub.add_parser("provenance", help="print a pinned bundle's build provenance")
    prov_p.add_argument("name")
    a = ap.parse_args(argv)
    if a.cmd == "provenance":
        print(json.dumps(provenance(a.name), indent=1))
        return 0
    if a.cmd == "latest":
        got = latest(Path(a.dest), a.lock)
        Path(a.out).write_text(json.dumps(got, indent=1, sort_keys=True), encoding="utf-8")
        for n, r in sorted(got.items()):
            print(f"{n}: {r['version']} ({r['src']}){'  ' + r['error'] if 'error' in r else ''}")
        return 0
    if a.cmd == "fetch":
        fetch(Path(a.dest), a.lock)
        os.environ["DVS_PERF_TOOLS"] = a.dest
    if a.lock == "sim":
        want = {n for n, _, _ in _pinned("sim")}
    else:
        want = needed_for(a.suites.split(",")) if getattr(a, "suites", None) else \
            {*SOLVERS, "yosys", "yosys-abc"}
    print(json.dumps(check(sorted(want))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
