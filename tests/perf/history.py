"""Committed trend history plus the artifacts not yet consolidated (design §4).

    python3 -m tests.perf.history status [--warn-after 60] [--fail-after 80]
    python3 -m tests.perf.history fetch --out DIR

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
    for n, text in enumerate(p.read_text().splitlines(), 1):
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


def fetch(out: Path, lines: list = None) -> list:
    """Download every unconsolidated record into out/; return [(artifact, record)]."""
    fj = Forgejo()
    lines = committed_lines() if lines is None else lines
    got = []
    out.mkdir(parents=True, exist_ok=True)
    for name, aid in unconsolidated(fj.perf_artifacts(), lines):
        for fn, rec in fj.records(aid):
            with gzip.GzipFile(out / Path(fn).name, "wb", mtime=0) as f:
                f.write(json.dumps(rec, sort_keys=True).encode())
            got.append((name, rec))
    return got


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="perf history: status of, or fetch, unconsolidated runs")
    sub = ap.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("status")
    st.add_argument("--warn-after", type=float, default=60)
    st.add_argument("--fail-after", type=float, default=None)
    fe = sub.add_parser("fetch")
    fe.add_argument("--out", required=True)
    a = ap.parse_args(argv)

    lines = committed_lines()
    try:
        if a.cmd == "fetch":
            got = fetch(Path(a.out), lines)
            print(f"fetched {len(got)} unconsolidated run record(s)")
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
