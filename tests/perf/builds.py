"""The dv-solve builds a run measures (design §5.2).

`head` is the checkout's own build (build/dv-solve-smt2 unless --head-bin
says otherwise). The anchor and ladder builds are release tags compiled from
the tag's source: the published wheels carry only the libraries, not the
dv-solve-smt2 CLI, and the CLI target builds in a couple of seconds.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
ANCHOR = "v0.1.0"
TAG_REMOTE = os.environ.get("DVS_PERF_TAG_REMOTE", "https://github.com/fvutils/dv-solve")


def _git(*args, cwd=_REPO, **kw):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True, **kw)


def tag_commit(tag: str) -> str:
    try:
        return _git("rev-list", "-n", "1", tag).stdout.strip()
    except subprocess.CalledProcessError:
        pass
    # CI checkouts are shallow and carry no tags, and the Forgejo mirror lacks
    # the newer ones: try origin, then the GitHub repo the tags are made in.
    for remote in ("origin", TAG_REMOTE):
        try:
            _git("fetch", "-q", "--depth", "1", remote, f"refs/tags/{tag}:refs/tags/{tag}")
            return _git("rev-list", "-n", "1", tag).stdout.strip()
        except subprocess.CalledProcessError:
            continue
    raise RuntimeError(f"release tag {tag} not found on origin or {TAG_REMOTE}")


def build_tag(tag: str, root: Path, jobs: int = 8) -> Path:
    """Build `tag`'s dv-solve-smt2 under root/<tag>/; reuse an existing build."""
    src = root / tag
    exe = src / "build" / "dv-solve-smt2"
    if exe.exists():
        return exe
    tag_commit(tag)
    src.mkdir(parents=True, exist_ok=True)
    archive = subprocess.run(["git", "archive", tag], cwd=_REPO, check=True,
                             capture_output=True).stdout
    subprocess.run(["tar", "x", "-C", str(src)], input=archive, check=True)
    # A tag's tree has no packages/: point it at the checkout's (cadical,
    # needed from v0.2.0 on), which also beats a stray $IVPM_PACKAGES.
    pkgs = [f"-DPACKAGES_DIR={_REPO / 'packages'}"] if (_REPO / "packages").is_dir() else []
    subprocess.run(["cmake", "-S", str(src), "-B", str(src / "build"), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release", *pkgs], check=True, capture_output=True)
    subprocess.run(["cmake", "--build", str(src / "build"), f"-j{jobs}",
                    "--target", "dv-solve-smt2"], check=True, capture_output=True)
    return exe


def head_bin(override: str = None) -> Path:
    p = Path(override) if override else _REPO / "build" / "dv-solve-smt2"
    if not p.exists():
        raise RuntimeError(f"head build not found: {p} (cmake --build build --target dv-solve-smt2)")
    return p
