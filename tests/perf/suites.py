"""Suite manifests: tests/perf/suites/<suite>.json → resolved fixture lists.

JSON rather than TOML: the runner's Python 3.10 has no tomllib. A resolved
manifest records every fixture's sha256, and its hash is the identity trend
lines are aligned to, so a regenerated fixture shows up as a new manifest
instead of silently changing what a trend line compares (design §2.2).
"""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
_LOGIC = re.compile(rb"\(\s*set-logic\s+([A-Z_]+)\s*\)")


def load(name: str) -> dict:
    """The suite spec plus `fixtures` [{path, sha, cat, logic}] and `hash`.

    A randomization suite (`"kind": "rand"`) lists benchmarks defined in
    tests/perf/rand_benches.py instead of fixture files; see load_rand()."""
    spec = json.loads((_HERE / "suites" / f"{name}.json").read_text(encoding="utf-8"))
    if spec.get("kind") == "rand":
        return load_rand(spec)
    fixtures = []
    for c in spec["categories"]:
        for p in sorted(_REPO.glob(c["glob"])):
            data = p.read_bytes()
            m = _LOGIC.search(data)
            fixtures.append({"path": str(p.relative_to(_REPO)), "cat": c["name"],
                             "sha": hashlib.sha256(data).hexdigest()[:16],
                             "logic": m.group(1).decode() if m else None})
    spec["fixtures"] = fixtures
    spec["hash"] = manifest_hash(spec)
    return spec


def manifest_hash(spec: dict) -> str:
    """Identity of what is compared: the fixtures and their categories, not the weights."""
    key = json.dumps([(f["path"], f["sha"], f["cat"]) for f in spec["fixtures"]])
    return hashlib.sha256(key.encode()).hexdigest()[:12]


def manifest_record(spec: dict) -> dict:
    """What a run record stores: enough to re-render without the checkout."""
    return {"hash": spec["hash"], "suite": spec["suite"],
            "weights": {c["name"]: c["weight"] for c in spec["categories"]},
            "about": {c["name"]: c.get("about", "") for c in spec["categories"]},
            "fixtures": [{k: f[k] for k in ("path", "sha", "cat")} for f in spec["fixtures"]]}


def load_rand(spec: dict) -> dict:
    """A randomization suite: `benches` [{name, n, sha, about, space}] and `hash`.

    `sha` is the benchmark's own (variables, constraints, bins); `space` is the
    number of solutions when enumerable, else None (binned)."""
    from .rand_benches import Bench
    from tests.formal.soundness.ir import solutions
    out = []
    for e in spec["benches"]:
        b = Bench(e["name"])
        space = None
        if b.bins is None:
            space = sum(1 for _ in solutions(b.widths, b.cons))
        out.append({"name": b.name, "n": e.get("n", spec["n"]), "sha": b.sha(),
                    "about": b.about, "space": space,
                    "bins": len(b.bins[1]) if b.bins else None})
    spec["benches"] = out
    key = json.dumps([(x["name"], x["sha"], x["n"]) for x in out])
    spec["hash"] = hashlib.sha256(key.encode()).hexdigest()[:12]
    return spec
