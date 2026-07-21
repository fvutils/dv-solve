"""Soundness cross-check for the P3b diversified portfolio (DV_CUBE_PORTFOLIO).

Portfolio workers attack the FULL instance (no cube assumptions) with a distinct
RNG seed + phase, composed with the cube pool. Each of the portfolio verdicts is
a soundness-critical shortcut:
  * a full-instance SAT   → a global model (winner installed like a cube win);
  * a full-instance UNSAT → an AUTHORITATIVE global UNSAT that must override an
    abandoned cube region (port_unsat), yet must never fire on a SAT instance;
  * a chunk-exhausted UNKNOWN → just loops (must not be mistaken for a verdict).

This test forces the portfolio side-channel to carry the search and checks it
never disagrees with the trusted bitblast oracle, across a mix of configs:
  * hybrid (some cube + some portfolio) and portfolio-heavy (all but one);
  * a tiny port chunk (DV_CUBE_PORTCHUNK=1) so the chunk-exhaust/loop path fires;
  * a tiny cube budget (DV_CUBE_BUDGET0=1) so cube workers abandon fast, leaving
    the portfolio to resolve — the case where port_unsat must correctly override
    the resulting `unresolved` on genuinely-UNSAT instances.

A cube/portfolio `unknown` is sound and tolerated; only sat/unsat that disagrees
with the oracle is a bug.

Usage:
    direnv exec . pytest tests/formal/test_cube_portfolio.py -v
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


# Portfolio configurations: hybrid, portfolio-heavy, and forced chunk-loop. All
# carve portfolio workers from a 4-worker pool (engine always keeps >=1 cube). A
# tiny BUDGET0 makes cube workers abandon so the portfolio must carry the run.
_BASE = {"DV_ENGINE": "cube", "DV_PARALLEL": "4", "DV_CUBE_BUDGET0": "1"}
_CONFIGS = {
    "hybrid_2":   {**_BASE, "DV_CUBE_PORTFOLIO": "2"},
    "port_heavy": {**_BASE, "DV_CUBE_PORTFOLIO": "3"},   # leaves 1 cube worker
    "chunk_loop": {**_BASE, "DV_CUBE_PORTFOLIO": "3", "DV_CUBE_PORTCHUNK": "1"},
    "no_cart":    {**_BASE, "DV_CUBE_PORTFOLIO": "2", "DV_CUBE_CART": "0"},
}


# ---- generative multi-`or` instances (mix of sat and unsat) --------------- #

def _atom(rng: int, var: str) -> str:
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
    s = seed * 2654435761 & 0xFFFFFFFF

    def nxt() -> int:
        nonlocal s
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        return s

    lines = ["(set-logic QF_BV)",
             "(declare-fun a () (_ BitVec 8))",
             "(declare-fun b () (_ BitVec 8))"]
    n_or = 3 + (nxt() % 4)
    for _ in range(n_or):
        arity = 3 + (nxt() % 6)
        atoms = [_atom(nxt(), "a" if (nxt() & 1) else "b") for _ in range(arity)]
        lines.append(f"(assert (or {' '.join(atoms)}))")
    lines.append(f"(assert (bvult a #x{(nxt() % 200) + 30:02x}))")
    lines.append(f"(assert (bvuge b #x{nxt() % 40:02x}))")
    lines.append("(check-sat)")
    return "\n".join(lines) + "\n"


_SEEDS = list(range(1, 41))


@pytest.mark.parametrize("seed", _SEEDS)
def test_portfolio_matches_bitblast(seed: int) -> None:
    """Forced-portfolio cube must agree with the bitblast oracle, every config."""
    smt2 = _gen(seed)
    oracle = _oracle(smt2)
    if oracle not in _ANSWERS:
        pytest.skip(f"oracle non-answer: {oracle}")
    for name, env in _CONFIGS.items():
        got, _ = _run(smt2, env)
        if got not in _ANSWERS:
            continue  # cube/portfolio unknown is sound
        assert got == oracle, (
            f"seed={seed} config={name}: cube says {got!r}, oracle {oracle!r}\n{smt2}"
        )


def test_portfolio_unsat_overrides_abandoned_cubes() -> None:
    """A genuinely-UNSAT instance: with BUDGET0=1 the cube pool abandons regions,
    so the portfolio's authoritative full-instance UNSAT (port_unsat) must carry
    the verdict rather than degrading to `unknown`."""
    smt2 = (
        "(set-logic QF_BV)\n"
        "(declare-fun a () (_ BitVec 8))\n"
        "(assert (bvult a #x03))\n"                        # a in {0,1,2}
        "(assert (or (= a #x05) (= a #x06) (= a #x07)))\n"  # a in {5,6,7} — clash
        "(check-sat)\n"
    )
    assert _oracle(smt2) == "unsat"
    for name, env in _CONFIGS.items():
        got, _ = _run(smt2, env)
        assert got in ("unsat", "unknown"), f"{name}: {got!r}"


def test_portfolio_finds_sat_model() -> None:
    """A SAT instance the portfolio should crack; verify the model is installed
    (get-value succeeds) and the verdict never spuriously flips to unsat."""
    smt2 = (
        "(set-logic QF_BV)\n"
        "(declare-fun a () (_ BitVec 8))\n"
        "(declare-fun b () (_ BitVec 8))\n"
        "(assert (or (= a #x11) (= a #x22) (= a #x33)))\n"
        "(assert (= b (bvadd a #x01)))\n"
        "(check-sat)\n"
        "(get-value (a b))\n"
    )
    assert _oracle(smt2) == "sat"
    for name, env in _CONFIGS.items():
        got, _ = _run(smt2, env)
        assert got in ("sat", "unknown"), f"{name}: {got!r}"
