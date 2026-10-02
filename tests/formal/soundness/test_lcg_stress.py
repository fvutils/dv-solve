"""Clause-learning stress problems: each drives many conflicts or once exposed
an unsound learning step (B50, B51, B54-B56).

The answer must match z3. With a step-checker build in build-check/
(-DDVS_STEP_CHECK=ON), every learnt clause and explanation on the way is
checked too: an unsound step that happens not to change the answer still
fails here.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

from .ir import _z3

_REPO = Path(__file__).resolve().parents[3]
_EXE = _REPO / "build" / "dv-solve-smt2"
_CHECK = _REPO / "build-check" / "dv-solve-smt2"
_FILES = sorted((Path(__file__).parent / "lcg_stress").glob("*.smt2"))


def _answer(exe, path, env=None):
    r = subprocess.run([str(exe), str(path)], capture_output=True, text=True, timeout=120,
                       env=env)
    words = r.stdout.split()
    return (words[0] if words else "error"), r.stderr


@pytest.mark.skipif(not _EXE.is_file() or _z3() is None, reason="dv-solve-smt2 or z3 missing")
@pytest.mark.parametrize("path", _FILES, ids=lambda p: p.stem)
def test_answer_matches_z3(path):
    z = subprocess.run([_z3(), str(path)], capture_output=True, text=True,
                       timeout=120).stdout.split()[:1]
    assert [_answer(_EXE, path)[0]] == z


@pytest.mark.skipif(not _CHECK.is_file(), reason="no step-checker build in build-check/")
@pytest.mark.parametrize("path", _FILES, ids=lambda p: p.stem)
def test_every_learning_step_is_sound(path):
    env = dict(os.environ, DV_STEP_CHECK_CONTINUE="1")
    _, err = _answer(_CHECK, path, env)
    assert "[step-check] INVALID" not in err, err[err.find("[step-check] INVALID"):][:2000]
