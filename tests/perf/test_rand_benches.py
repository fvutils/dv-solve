"""Every rand_benches.py entry constructs, and the solver's own draws satisfy it.

construct: `Bench(name)` builds for every ENUMERABLE/WIDE entry, and its
`smt_vlt()`/`sha()` helpers don't raise.
bins: a WIDE bench's `bins` truth table sums to 1 (a typo in the hand-written
share arithmetic would otherwise silently skew every sampler's score).
draws: seeded solves of the new item-0/4/5 benches, through the same builder
path `tests/perf/run_rand.py`'s `dv-api` arm uses, come back `SOLVE_OK` and
satisfy their constraints via the IR's own evaluator (`ev`), not the
builder's -- this is what the manual tuple check in the outline did not
exercise: `bvurem`, `bvxor`, `bvmul` and 64-bit extract through the real C
solver.
"""
from __future__ import annotations

import os
import random
from pathlib import Path

import pytest

dv = pytest.importorskip("dv_solve")

from tests.perf.rand_benches import Bench, ENUMERABLE, WIDE  # noqa: E402

_REPO = Path(__file__).resolve().parents[2]

# Point the native loader at the locally-built profiling library when no
# installation is already selected (DVS_SOLVER_PATH / an installed package /
# a `build` directory): this file runs against `build-prof`, which the
# resolver does not search by name.
if not os.environ.get("DVS_SOLVER_PATH"):
    _lib_dir = _REPO / "build-prof" / "lib"
    if (_lib_dir / "libdv_solve.so").exists():
        os.environ["DVS_SOLVER_PATH"] = str(_lib_dir)

ALL_NAMES = sorted(set(ENUMERABLE) | set(WIDE))
NEW_WIDE = ["cond_weight", "order_chain", "linear_sys", "mod_crt",
            "xor_split", "pop32", "align64"]


@pytest.mark.parametrize("name", ALL_NAMES)
def test_bench_constructs(name):
    b = Bench(name)
    assert b.names
    assert all(isinstance(s, str) for s in b.smt_vlt())
    assert len(b.sha()) == 16


@pytest.mark.parametrize("name", sorted(WIDE))
def test_bins_sum_to_one(name):
    b = Bench(name)
    assert b.bins is not None
    key, truth = b.bins
    assert callable(key)
    assert sum(truth.values()) == pytest.approx(1.0, abs=1e-9)


def _draw_and_check(name: str, n: int, seed: int) -> None:
    from dv_solve.builder import SolveProblemBuilder
    from dv_solve.ctx import SOLVE_OK, SolveCtx
    from tests.formal.soundness.doors import _lower
    from tests.formal.soundness.ir import ev

    b = Bench(name)
    ids = {k: i for i, k in enumerate(b.names)}
    bld = SolveProblemBuilder()
    for k in b.names:
        w = b.widths[k]
        bld.add_var(ids[k], width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
    for c in b.cons:
        bld.add_constraint(_lower(bld, c, ids))
    prob, _ = bld.finalize()
    rng = random.Random(seed)
    with SolveCtx(prob) as ctx:
        for _ in range(n):
            ctx.reset()
            rc = ctx.solve(seed=rng.getrandbits(62) + 1, time_limit_ms=5000)
            assert rc == SOLVE_OK, f"{name}: solve returned {rc}"
            m = {k: ctx.get_value(ids[k]) & ((1 << b.widths[k]) - 1) for k in b.names}
            assert all(ev(c, m) for c in b.cons), (name, m)
            assert ctx.validate_model() == 0, (name, m)


@pytest.mark.parametrize("name", NEW_WIDE)
def test_new_wide_draws_are_valid(name):
    _draw_and_check(name, n=20, seed=1)
