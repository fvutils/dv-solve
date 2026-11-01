"""The svcorpus stats hold no names (solve stats plan, S2).

Every case is solved with DV_STATS on; the stats file and its bundles (salted
and not) pass the audit, with every case and field name in the corpus as the
words to look for.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

from tests.perf.svcorpus import case as C

dv = pytest.importorskip("dv_solve")
from dv_solve import stats as S  # noqa: E402

REPO = Path(__file__).resolve().parents[3]

_RUN = r"""
from tests.perf.svcorpus import case as C
from dv_solve.ctx import SolveCtx
for name in C.names():
    try:
        ctx = SolveCtx(C.problem(C.load(name)))
    except Exception:
        continue                       # CDCL cannot take it: no solves
    with ctx:
        for s in range(5):
            ctx.reset()
            ctx.solve(seed=s + 1)
"""


def _words():
    words = set(C.names())
    for name in C.names():
        case = C.load(name)
        for n, _, _ in case.VARS:
            words.add(n)
            words.update(p for p in n.replace("[", " ").replace("]", " ").replace(".", " ")
                         .split() if not p.isdigit())
        words.update(getattr(case, "DIST", {}))
    return sorted(w for w in words if len(w) >= 3)


def test_no_names_in_stats(tmp_path):
    out = tmp_path / "svcorpus.json"
    env = dict(os.environ, DV_STATS=str(out))
    env.pop("LD_LIBRARY_PATH", None)
    env["PYTHONPATH"] = os.pathsep.join([str(REPO / "src"), str(REPO), env.get("PYTHONPATH", "")])
    r = subprocess.run([sys.executable, "-c", _RUN], env=env, cwd=REPO,
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, r.stderr
    data = S.load(str(out))
    assert len(data["snapshot"]["classes"]) >= 10
    words = _words()
    assert len(words) > 100
    assert S.audit(data, words, out.read_text()) == []
    for salt in (b"k" * 32, None):
        b = S.bundle(S.merge([data]), salt=salt)
        assert S.audit(b, words) == []
