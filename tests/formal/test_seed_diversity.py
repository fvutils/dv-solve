"""Seeded sampling over plain SMT-LIB2 (docs/fw_hdl_implementation_plan.md
piece 6.4).

fw-hdl draws stimulus by running one script per seed with
`(set-option :random-seed k)` and reading the model. Each case here does the
same over a benchmark of tests/perf/rand_benches.py (the `seed` arm of the
rand-core suite) and checks that every solution comes up, and that the counts
pass a chi-square test against a uniform draw.

`crc_filter` is fw-hdl's own check: 'A'..'Z' with crc32(m)[1:0] == 0, 6
solutions. Before 6.4 the bit-blast engine answered it with the lowest
solution its exhaustive simulation found, and so did `countones` and
`sum_eq_const`: a few answers whatever the seed. The others went to CDCL,
whose decisions skew orderings and disjunctions (`a_lt_b`: `a == 0` at half
its share). Now a seeded script goes to bit-blast, whose simulation draws
uniformly (perf-out/p64/route_ab.log).

The seeds are fixed, so a build gives the same counts every run.
"""
from __future__ import annotations

import os
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
# DV_SOLVE_SMT2: test another build.
_EXE = Path(os.environ.get("DV_SOLVE_SMT2", _REPO / "build" / "dv-solve-smt2"))
pytestmark = pytest.mark.skipif(not _EXE.is_file(), reason="build/dv-solve-smt2 not built")

sys.path.insert(0, str(_REPO))
from tests.formal.soundness.ir import ev, solutions  # noqa: E402
from tests.perf import vlt_protocol  # noqa: E402
from tests.perf.rand_benches import Bench  # noqa: E402
from tests.perf.run_rand import seed_script  # noqa: E402


def _draw(b: Bench, k: int) -> tuple:
    import subprocess
    env = {n: v for n, v in os.environ.items() if n != "LD_LIBRARY_PATH"}
    p = subprocess.run([str(_EXE)], input=seed_script(b, k), capture_output=True,
                       text=True, timeout=30, env=env)
    assert p.stdout.startswith("sat"), (k, p.stdout, p.stderr)
    m = {n: vlt_protocol._value(lit, dec) for n, lit, dec in vlt_protocol._VAL.findall(p.stdout)}
    assert all(ev(c, m) for c in b.cons), (k, m)
    return tuple(m[n] for n in b.names)


# bench, draws: ten per solution where that is cheap, two for the larger
# spaces (chi-square still holds at that many degrees of freedom).
@pytest.mark.parametrize("name, n", [
    ("crc_filter", 60),         # fw-hdl's 60 seeds
    ("crc_filter", 600),
    ("countones", 700),         # 70 solutions
    ("sum_eq_const", 2560),     # 256
    ("bit_mask", 160),          # 16
    ("disjoint_ranges", 620),   # 62, two ranges
    ("dep_range", 2000),        # 891, a range that depends on another variable
    ("a_lt_b", 4032),           # 2016, an ordering
    ("ot_aon_wkup", 2006),      # 1003, one of them alone in its branch
])
def test_seeded_draws_are_uniform(name: str, n: int) -> None:
    scipy = pytest.importorskip("scipy.stats")
    b = Bench(name)
    space = {tuple(m[k] for k in b.names) for m in solutions(b.widths, b.cons)}
    with ThreadPoolExecutor(8) as ex:
        draws = list(ex.map(lambda k: _draw(b, k), range(1, n + 1)))
    cnt = Counter(draws)
    assert set(cnt) <= space
    if n >= 10 * len(space) or name == "crc_filter":
        assert set(cnt) == space, f"{name}: {len(cnt)} of {len(space)} solutions in {n} draws"
    exp = n / len(space)
    chi2 = sum((cnt.get(s, 0) - exp) ** 2 / exp for s in space)
    p = scipy.chi2.sf(chi2, len(space) - 1)
    assert p > 1e-3, f"{name}: chi-square p = {p:.2g} over {n} draws"
