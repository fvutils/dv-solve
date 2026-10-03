"""Replay every shrunk failure the soundness campaign has recorded.

Each regressions/*.json holds a problem small enough to enumerate; it must
now give the brute-force answer (and a valid model) through every front door
it can use. Add a file by running the campaign; see campaign.py.
"""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from .campaign import DEFAULT_EXE, failing, from_json, run_doors

_FILES = sorted((Path(__file__).parent / "regressions").glob("*.json"))


@pytest.mark.skipif(not DEFAULT_EXE.is_file(), reason="dv-solve-smt2 not built")
@pytest.mark.parametrize("path", _FILES, ids=lambda p: p.stem)
def test_regression(path):
    d = json.loads(path.read_text())
    p = from_json(d)
    bad = failing(run_doors(p, ["smt2", "incr", "steps", "builder", "protocol"], str(DEFAULT_EXE)))
    assert not bad, (d["why"], [(o.door, o.expect, o.got, o.model_ok) for o in bad])
