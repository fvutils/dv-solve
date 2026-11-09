"""Power-of-two divisors and factors in the bit-blast and CDCL engines
(docs/structural_reasoning_design.md, rewrites A and B and step 3).

Each fixture in smt2/pow2 is a shape seen in bus and DMA testbenches: a
divisor or factor that is a constant power of two, `1 << k`, a variable
confined to powers of two (`inside {1,2,4,8}`, `(b & (b-1)) == 0`). For each:

  * the bit-blast engine answers the expected result (the file name's
    ``_sat``/``_unsat``, which z3 confirms);
  * its CNF stays within a clause budget, so a regression to the general
    multiplier or divider (10-20x the clauses) fails here rather than only
    slowing down;
  * pure CDCL answers it on each of three seeds within 5 s (test_pow2_cdcl).
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_EXE = _REPO / "build" / "dv-solve-smt2"
_DIR = Path(__file__).resolve().parent / "smt2" / "pow2"

# Budget per fixture: about twice the shift encoding's count. The general
# encoding, in parentheses, is what a lost fact falls back to.
_BUDGET = {
    "axi_align_4k_64_unsat": 17_000,     # (52,267)
    "axi_burst_bytes_64_sat": 18_000,    # (53,029)
    "mul_pow2var_32_sat": 5_000,         # (12,287)
    "mul_pow2var_64_sat": 11_000,        # (49,148)
    "onehot_mul_32_sat": 5_000,          # (12,179)
    "onehot_mul_64_sat": 10_000,         # (48,753)
    "pow_int_exp_signed_sat": 3_500,     # (33,367)
    "sdiv_const4_64_sat": 2_000,         # (2,697)
    "sdiv_pow2var_64_sat": 22_000,       # (137,336)
    "udiv_pow2set_32_sat": 5_000,        # (32,750)
    "udiv_pow2set_64_sat": 19_000,       # (181,834)
}

_FILES = sorted(_DIR.glob("*.smt2")) if _DIR.is_dir() else []


def _expected(path: Path) -> str:
    return "unsat" if path.stem.endswith("_unsat") else "sat"


@pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built")
@pytest.mark.parametrize("smt2", _FILES, ids=[f.stem for f in _FILES])
def test_pow2_bitblast(smt2: Path) -> None:
    env = dict(os.environ, DV_BB_STATS="1")
    p = subprocess.run([str(_EXE), "--engine=bitblast", str(smt2)],
                       capture_output=True, text=True, timeout=60, env=env)
    out = p.stdout.split()
    assert out and out[0] == _expected(smt2), (p.stdout, p.stderr[-2000:])
    budget = _BUDGET.get(smt2.stem)
    if budget is None:
        return
    m = re.search(r"clauses=(\d+)", p.stderr)
    assert m, p.stderr[-2000:]
    assert int(m.group(1)) <= budget, f"{m.group(1)} clauses > budget {budget}"


# Pure CDCL (structural_reasoning_design.md, step 3): every fixture solves in
# milliseconds on every seed; before step 3 six of them timed out and
# axi_burst_bytes_64 returned unknown. A regression shows as `unknown` within
# the budget below.


@pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built")
@pytest.mark.parametrize("seed", [1, 2, 3])
@pytest.mark.parametrize("smt2", _FILES, ids=[f.stem for f in _FILES])
def test_pow2_cdcl(smt2: Path, seed: int) -> None:
    env = dict(os.environ, DV_NO_BITBLAST="1", DV_CDCL_TIME_LIMIT="5")
    src = f"(set-option :random-seed {seed})\n" + smt2.read_text()
    p = subprocess.run([str(_EXE), "--engine=cdcl"], input=src, capture_output=True,
                       text=True, timeout=60, env=env)
    out = p.stdout.split()
    assert out and out[0] == _expected(smt2), (p.stdout, p.stderr[-2000:])
