"""Committed trend history plus the artifacts not yet consolidated (design §4).

    python3 -m tests.perf.history status [--warn-after 60] [--fail-after 80]
    python3 -m tests.perf.history fetch --out DIR [--latest]
    python3 -m tests.perf.history measured --commit SHA --kind nightly [--max-age-days 7]

Forgejo access comes from the environment, never from a tracked file:
  FORGEJO_API_URL   e.g. <server>/api/v1   (falls back to GITHUB_API_URL)
  FORGEJO_TOKEN     read access to the repo's artifacts (falls back to GITHUB_TOKEN)
  GITHUB_REPOSITORY owner/name, default fvutils/dv-solve
Anonymous reads of the artifact API are refused (403), so a token is needed.
Nothing here prints a URL: job logs of a public repo are public.
"""
from __future__ import annotations

import argparse
import datetime
import gzip
import io
import json
import os
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

from . import schema

HISTORY = Path(__file__).resolve().parent / "history"


class AccessError(RuntimeError):
    pass


def committed_lines_from(p: Path) -> list:
    lines = []
    for n, text in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
        if text.strip():
            try:
                lines.append(json.loads(text))
            except json.JSONDecodeError as e:
                raise ValueError(f"{p.name}:{n}: {e}") from None
    return lines


def committed_lines(root: Path = HISTORY) -> list:
    """Every trend line in <root>/*.jsonl, in file order."""
    return [l for p in sorted(root.glob("*.jsonl")) for l in committed_lines_from(p)]


def newest_utc(lines: list) -> str:
    return max((l["utc"] for l in lines), default="")


def _utc_age_days(utc: str) -> float:
    t = datetime.datetime.strptime(utc, "%Y%m%dT%H%M%SZ").replace(tzinfo=datetime.timezone.utc)
    return (datetime.datetime.now(datetime.timezone.utc) - t).total_seconds() / 86400


class Forgejo:
    def __init__(self):
        self.api = (os.environ.get("FORGEJO_API_URL") or os.environ.get("GITHUB_API_URL") or "").rstrip("/")
        self.token = os.environ.get("FORGEJO_TOKEN") or os.environ.get("GITHUB_TOKEN") or ""
        self.repo = os.environ.get("GITHUB_REPOSITORY") or "fvutils/dv-solve"
        if not self.api:
            raise AccessError("FORGEJO_API_URL is not set")

    def _get(self, path: str) -> bytes:
        req = urllib.request.Request(f"{self.api}/repos/{self.repo}/{path}")
        if self.token:
            req.add_header("Authorization", f"token {self.token}")
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            raise AccessError(f"artifact API answered HTTP {e.code} for {path.split('?')[0]}") from None
        except (urllib.error.URLError, OSError) as e:
            # The reason can name the host; keep it out of the log.
            raise AccessError(f"artifact API unreachable ({type(e).__name__})") from None

    def perf_artifacts(self) -> list:
        """Unexpired perf-* artifacts, oldest first: [(name, id)]."""
        found, page = {}, 1
        while True:
            batch = json.loads(self._get(f"actions/artifacts?limit=50&page={page}"))
            if isinstance(batch, dict):          # per-run endpoint shape
                batch = batch.get("artifacts") or []
            if not batch:
                break
            for a in batch:
                if not a.get("expired") and schema.ARTIFACT_RE.match(a.get("name", "")):
                    found.setdefault(a["name"], a["id"])   # newest upload of a name wins
            page += 1
        return sorted(found.items())

    def records(self, art_id: int) -> list:
        """The run records inside one artifact zip: [(filename, record)]."""
        out = []
        with zipfile.ZipFile(io.BytesIO(self._get(f"actions/artifacts/{art_id}/zip"))) as z:
            for n in z.namelist():
                if n.endswith(".json.gz"):
                    out.append((n, json.loads(gzip.decompress(z.read(n)))))
        return out


def unconsolidated(arts: list, lines: list) -> list:
    """Artifacts newer than the newest committed trend line."""
    cut = newest_utc(lines)
    return [(n, i) for n, i in arts if schema.ARTIFACT_RE.match(n)["utc"] > cut]


def _has_sat(rec: dict) -> bool:
    return bool(rec.get("valid") and rec.get("sat"))


def fetch(out: Path, lines: list = None, latest: bool = False) -> list:
    """Download every unconsolidated record into out/; return [(artifact, record)].

    latest: also make sure the newest valid record with SAT results is there,
    even if it was consolidated already (the pages show its full detail, which
    the committed trend lines do not keep). Looks back at most 10 artifacts.
    """
    fj = Forgejo()
    lines = committed_lines() if lines is None else lines
    arts = fj.perf_artifacts()
    got = []
    out.mkdir(parents=True, exist_ok=True)

    def take(name, aid):
        for fn, rec in fj.records(aid):
            with gzip.GzipFile(out / Path(fn).name, "wb", mtime=0) as f:
                f.write(json.dumps(rec, sort_keys=True).encode())
            got.append((name, rec))

    pending = unconsolidated(arts, lines)
    for name, aid in pending:
        take(name, aid)
    if latest and not any(_has_sat(r) for _, r in got):
        older = [a for a in arts if a not in pending]
        for name, aid in list(reversed(older))[:10]:
            take(name, aid)
            if _has_sat(got[-1][1]):
                break
    return got


def measured(commit: str, kind: str, max_age_days: float) -> bool:
    """A valid `kind` record for `commit`, younger than max_age_days, exists."""
    for l in committed_lines():
        if (l["commit"].startswith(commit[:7]) and l["kind"] == kind and l["valid"]
                and _utc_age_days(l["utc"]) < max_age_days):
            return True
    fj = Forgejo()
    for name, aid in fj.perf_artifacts():
        m = schema.ARTIFACT_RE.match(name)
        if m["kind"] == kind and commit.startswith(m["sha"]) and _utc_age_days(m["utc"]) < max_age_days:
            if any(r.get("valid") for _, r in fj.records(aid)):
                return True
    return False


# Releases are tagged on GitHub; the Forgejo mirror does not receive new tags
# (its sync blocks tags in repos with a tag-publish workflow), so the list of
# releases comes from there. Public, read-only.
from .builds import TAG_REMOTE


def _vkey(tag: str) -> tuple:
    try:
        return tuple(int(x) for x in tag[1:].split("."))
    except ValueError:
        return (1 << 30,)


def release_tags(remote: str = TAG_REMOTE) -> list:
    """v* tags of `remote`, oldest version first: [(tag, commit)]."""
    import subprocess
    try:
        out = subprocess.run(["git", "ls-remote", "--tags", remote], capture_output=True,
                             text=True, check=True, timeout=120).stdout
    except Exception as e:
        raise AccessError(f"cannot list release tags ({type(e).__name__})") from None
    tags = {}
    for row in out.splitlines():
        sha, ref = row.split("\t")
        tag = ref[len("refs/tags/"):]
        peeled = tag.endswith("^{}")
        tag = tag[:-3] if peeled else tag
        if tag.startswith("v") and (peeled or tag not in tags):
            tags[tag] = sha                  # the peeled (commit) sha wins
    return sorted(tags.items(), key=lambda t: _vkey(t[0]))


def release_pending(tags: list, lines: list, arts: list) -> list:
    """Release tags with no measurement yet: [(tag, commit)], oldest first.

    A release counts as measured when a committed trend line names it, or a
    committed line or an artifact of kind release or nightly is on its commit
    (a nightly on a release commit is that release's snapshot). A release
    artifact counts even when invalid, so a release that cannot be measured
    is not retried every night.
    """
    done = set()
    for l in lines:
        if l.get("release"):
            done.add(l["release"])
    commits = [l["commit"] for l in lines if l["kind"] in ("release", "nightly") and l["valid"]]
    commits += [schema.ARTIFACT_RE.match(n)["sha"] for n, _ in arts
                if schema.ARTIFACT_RE.match(n)["kind"] in ("release", "nightly")]
    return [(t, c) for t, c in tags
            if t not in done and not any(c.startswith(x) or x.startswith(c) for x in commits)]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="perf history: status of, or fetch, unconsolidated runs; releases not measured")
    sub = ap.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("status")
    st.add_argument("--warn-after", type=float, default=60)
    st.add_argument("--fail-after", type=float, default=None)
    fe = sub.add_parser("fetch")
    fe.add_argument("--out", required=True)
    fe.add_argument("--latest", action="store_true")
    me = sub.add_parser("measured")
    me.add_argument("--commit", required=True)
    me.add_argument("--kind", default="nightly")
    me.add_argument("--max-age-days", type=float, default=7)
    rp = sub.add_parser("release-pending", help="release tags not measured yet")
    rp.add_argument("--anchor-ok", action="store_true",
                    help="also list the anchor release (every run measures it as an arm)")
    a = ap.parse_args(argv)

    lines = committed_lines()
    try:
        if a.cmd == "fetch":
            got = fetch(Path(a.out), lines, a.latest)
            print(f"fetched {len(got)} run record(s)")
            return 0
        if a.cmd == "measured":
            yes = measured(a.commit, a.kind, a.max_age_days)
            print("measured" if yes else "not measured")
            if os.environ.get("GITHUB_OUTPUT"):
                with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as f:
                    f.write(f"measured={'true' if yes else 'false'}\n")
            return 0
        if a.cmd == "release-pending":
            from .builds import ANCHOR
            todo = release_pending(release_tags(), lines, Forgejo().perf_artifacts())
            if not a.anchor_ok:
                todo = [(t, c) for t, c in todo if t != ANCHOR]
            for t, c in todo:
                print(f"{t} {c}")
            print(f"{len(todo)} release(s) not measured")
            if os.environ.get("GITHUB_OUTPUT"):
                with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as f:
                    f.write(f"tag={todo[0][0] if todo else ''}\n")
            return 0
        pending = unconsolidated(Forgejo().perf_artifacts(), lines)
    except AccessError as e:
        print(f"::error::{e}")
        return 1
    print(f"committed trend lines: {len(lines)} (newest {newest_utc(lines) or 'none'})")
    if not pending:
        print("unconsolidated runs: 0")
        return 0
    oldest = _utc_age_days(schema.ARTIFACT_RE.match(pending[0][0])["utc"])
    msg = f"{len(pending)} runs unconsolidated, oldest {oldest:.0f} days"
    print(msg)
    if a.fail_after is not None and oldest > a.fail_after:
        print(f"::error::{msg}: consolidate now (python3 -m tests.perf.consolidate); "
              f"artifacts expire after about 90 days")
        return 1
    if oldest > a.warn_after:
        print(f"::warning::{msg}: consolidate soon")
    return 0


if __name__ == "__main__":
    sys.exit(main())
