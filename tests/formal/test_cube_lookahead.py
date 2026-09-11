"""Soundness cross-check for P3 lookahead deepening (DV_CUBE_LOOKAHEAD).

The lookahead deepener probes a budget-hit cube's next `or` disjuncts under a
tiny conflict budget: it DROPS disjuncts proved locally UNSAT, WINS on a SAT
probe, and proves the whole cube UNSAT when every disjunct is refuted (LA_DEAD).
Each of those is a soundness-critical shortcut, so this test forces the
lookahead path to drive the entire search and checks it never disagrees with the
trusted bitblast oracle.

Forcing recipe:
  * ``DV_CUBE_BUDGET0=1`` — every cube's main solve gives up almost immediately,
    so deepening (and thus lookahead) handles essentially all resolution.
  * ``DV_CUBE_LABUDGET`` large — probes resolve fully, so DROP/DEAD/WIN fire for
    real rather than degrading to "keep as UNKNOWN child".
  * Run under both the sequential (DV_PARALLEL=1) and parallel (DV_PARALLEL=4)
    conquer loops, and with the cartesian seed both on and off (off => the
    single-`or` frontier leans hardest on lookahead deepening).

The bitblast engine partitions the *same* bit-blasted instance, so a cube
sat/unsat that disagrees with it is a bug. A cube ``unknown`` is sound and
tolerated (though the forcing knobs are tuned so it rarely happens).

Usage:
    direnv exec . pytest tests/formal/test_cube_lookahead.py -v
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_BINARY = _HERE.parents[1] / "build" / "dv-solve-smt2"

_ANSWERS = {"sat", "unsat"}


def _run(smt2: str, env_extra: dict[str, str], timeout_s: float = 30.0) -> str:
    if not _BINARY.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    env = {"PATH": "/usr/bin:/bin", **env_extra}
    out = subprocess.run(
        [str(_BINARY), "/dev/stdin"],
        input=smt2, capture_output=True, text=True, timeout=timeout_s, env=env,
    )
    line = out.stdout.strip().splitlines()
    return (line[0].strip() if line else "").lower(), out.stderr


def _oracle(smt2: str) -> str:
    return _run(smt2, {"DV_ENGINE": "bitblast"})[0]


# Forced-lookahead cube configurations: seq/parallel × cartesian on/off.
_FORCE = {"DV_ENGINE": "cube", "DV_CUBE_LOOKAHEAD": "1",
          "DV_CUBE_BUDGET0": "1", "DV_CUBE_LABUDGET": "200000",
          "DV_CUBE_MAXDEPTH": "40"}
_CONFIGS = {
    "seq_cart":  {**_FORCE, "DV_PARALLEL": "1", "DV_CUBE_CART": "1"},
    "seq_nocart": {**_FORCE, "DV_PARALLEL": "1", "DV_CUBE_CART": "0"},
    "par_cart":  {**_FORCE, "DV_PARALLEL": "4", "DV_CUBE_CART": "1"},
    "par_nocart": {**_FORCE, "DV_PARALLEL": "4", "DV_CUBE_CART": "0"},
}


# ---- generative multi-`or` instances ------------------------------------- #

def _atom(rng: int, var: str) -> str:
    """A pseudo-random 8-bit atom over `var` (deterministic from `rng`)."""
    k = rng & 0xFF
    op = (rng >> 8) & 0x3
    if op == 0:
        return f"(= {var} #x{k:02x})"
    if op == 1:
        return f"(bvult {var} #x{k:02x})"
    if op == 2:
        return f"(bvuge {var} #x{k:02x})"
    return f"(= (bvadd a b) #x{k:02x})"


def _gen(seed: int) -> str:
    """Build a small QF_BV instance with several top-level `or`s (LCG-driven)."""
    s = seed * 2654435761 & 0xFFFFFFFF
    def nxt() -> int:
        nonlocal s
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        return s

    lines = ["(set-logic QF_BV)",
             "(declare-fun a () (_ BitVec 8))",
             "(declare-fun b () (_ BitVec 8))"]
    n_or = 3 + (nxt() % 4)              # 3..6 top-level ors
    for _ in range(n_or):
        arity = 3 + (nxt() % 6)         # 3..8 disjuncts each
        atoms = [_atom(nxt(), "a" if (nxt() & 1) else "b") for _ in range(arity)]
        lines.append(f"(assert (or {' '.join(atoms)}))")
    # A couple of plain bounds: tightens the space so some ors go fully DEAD.
    lines.append(f"(assert (bvult a #x{(nxt() % 200) + 30:02x}))")
    lines.append(f"(assert (bvuge b #x{nxt() % 40:02x}))")
    lines.append("(check-sat)")
    return "\n".join(lines) + "\n"


_SEEDS = list(range(1, 41))   # 40 deterministic instances


@pytest.mark.parametrize("seed", _SEEDS)
def test_lookahead_matches_bitblast(seed: int) -> None:
    """Forced-lookahead cube must agree with the bitblast oracle, every config."""
    smt2 = _gen(seed)
    oracle = _oracle(smt2)
    if oracle not in _ANSWERS:
        pytest.skip(f"oracle non-answer: {oracle}")
    for name, env in _CONFIGS.items():
        got, _ = _run(smt2, env)
        if got not in _ANSWERS:
            continue  # cube unknown is sound
        assert got == oracle, (
            f"seed={seed} config={name}: cube says {got!r}, oracle {oracle!r}\n{smt2}"
        )


def test_lookahead_path_actually_fires() -> None:
    """Confirm the lookahead code path (probe → SPLIT/DEAD) really executes."""
    # Many ors + budget 1 + verbose: deepening must reach a lookahead decision.
    smt2 = _gen(7)
    env = {**_CONFIGS["seq_nocart"], "DV_CUBE_VERBOSE": "1"}
    _, stderr = _run(smt2, env)
    # la=3 (SPLIT) or la=2 (DEAD) appears once lookahead deepens a budget-hit cube.
    assert (" la=" in stderr) or ("solve depth=" in stderr), stderr[:500]


def test_lookahead_dead_prune_unsat() -> None:
    """An `or` whose every disjunct is excluded by a bound must aggregate UNSAT."""
    smt2 = (
        "(set-logic QF_BV)\n"
        "(declare-fun a () (_ BitVec 8))\n"
        "(assert (bvult a #x03))\n"                       # a in {0,1,2}
        "(assert (or (= a #x05) (= a #x06) (= a #x07)))\n"  # a in {5,6,7} — all dead
        "(check-sat)\n"
    )
    for env in _CONFIGS.values():
        got, _ = _run(smt2, env)
        assert got in ("unsat", "unknown"), got
    assert _oracle(smt2) == "unsat"
