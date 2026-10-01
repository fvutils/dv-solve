"""A fixed-seed slice of the soundness campaign, on every run of the suite.

The nightly campaign runs fresh seeds at volume; this keeps a small,
deterministic sample in the ordinary test run so a regression in any front
door shows immediately. Every answer is judged against brute force.
"""
from __future__ import annotations

import pytest

from .campaign import DEFAULT_EXE, campaign


@pytest.mark.skipif(not DEFAULT_EXE.is_file(), reason="dv-solve-smt2 not built")
@pytest.mark.parametrize("seed", [11, 12])
def test_campaign_smoke(seed):
    failures, stats, _ = campaign(seed, 150, ["smt2", "incr", "builder"],
                                  str(DEFAULT_EXE), out_dir=None, log=lambda *_: None)
    assert not failures, [why for why, _ in failures]
