"""Run every example the documentation site shows.

The site pulls its code samples from docs/examples/ with literalinclude, so an
example that stops working here is an example that is wrong on the site.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_EXAMPLES = sorted((_REPO / "docs" / "examples").glob("*.py"))


def _env() -> dict:
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(
        [str(_REPO / "src")] + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else []))
    return env


@pytest.fixture(scope="module")
def example_env() -> dict:
    """Environment the examples run in, as a user would run them.

    The examples find libdv_solve through the package's own discovery, so skip
    (rather than fail) when discovery finds no built library.
    """
    env = _env()
    probe = subprocess.run(
        [sys.executable, "-c",
         "import sys; from dv_solve.lib import _load_lib; "
         "sys.exit(0 if _load_lib() is not None else 3)"],
        env=env, capture_output=True, timeout=60)
    if probe.returncode != 0:
        pytest.skip("libdv_solve not built; build it to run the doc examples")
    return env


@pytest.mark.parametrize("example", _EXAMPLES, ids=lambda p: p.name)
def test_doc_example_runs(example: Path, example_env: dict) -> None:
    proc = subprocess.run([sys.executable, str(example)], capture_output=True,
                          text=True, env=example_env, timeout=60)
    assert proc.returncode == 0, proc.stdout + proc.stderr


# ------------------------------------------------------------------ #
# SMT-LIB2 examples: each .smt2 has a .expected file holding the exact  #
# stdout the site shows. The default seed is fixed, so output is        #
# deterministic.                                                        #
# ------------------------------------------------------------------ #

_SMT2_EXAMPLES = sorted((_REPO / "docs" / "examples" / "smt2").glob("*.smt2"))


@pytest.fixture(scope="module")
def smt2_exe() -> str:
    import shutil
    exe = _REPO / "build" / "dv-solve-smt2"
    if exe.is_file():
        return str(exe)
    found = shutil.which("dv-solve-smt2")
    if found is None:
        pytest.skip("dv-solve-smt2 not built; build it to run the SMT-LIB2 doc examples")
    return found


@pytest.mark.parametrize("example", _SMT2_EXAMPLES, ids=lambda p: p.name)
def test_doc_smt2_example_output(example: Path, smt2_exe: str) -> None:
    proc = subprocess.run([smt2_exe, str(example)], capture_output=True,
                          text=True, timeout=60)
    assert proc.returncode in (0, 1), proc.stdout + proc.stderr
    assert proc.stdout == example.with_suffix(".expected").read_text(encoding="utf-8")


# The Verilator, DPI and C examples are built and run by test_doc_pages.py,
# using the commands their pages show.
