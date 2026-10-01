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
    assert proc.stdout == example.with_suffix(".expected").read_text()


# ------------------------------------------------------------------ #
# Verilator example: build docs/examples/verilator/packet.sv with the  #
# bundled (or PATH) Verilator, run it with dv-solve as the solver, and #
# check every printed packet against the class's constraints.          #
# ------------------------------------------------------------------ #

def test_doc_verilator_packet(smt2_exe: str, tmp_path: Path) -> None:
    import re
    import shutil
    vlt = _REPO / "packages" / "verilator-bin" / "bin" / "verilator"
    vlt = str(vlt) if vlt.is_file() else shutil.which("verilator")
    if vlt is None:
        pytest.skip("verilator not available")
    src = _REPO / "docs" / "examples" / "verilator" / "packet.sv"
    b = subprocess.run([vlt, "--binary", "--Mdir", str(tmp_path / "obj"),
                        "-o", "sim", str(src)], capture_output=True, text=True, timeout=900)
    assert b.returncode == 0, b.stdout[-2000:] + b.stderr[-2000:]
    env = dict(os.environ, VERILATOR_SOLVER=f"{smt2_exe} --interactive --mode=verilator")
    r = subprocess.run([str(tmp_path / "obj" / "sim")], capture_output=True, text=True,
                       env=env, timeout=120)
    out = r.stdout + r.stderr
    assert r.returncode == 0, out
    assert "Warning" not in out, out            # no solver errors, no unknowns
    pkts = re.findall(r"addr=([0-9a-f]+) len=(\d+) kind=(\d+)", out)
    assert len(pkts) == 5, out
    for a, n, k in pkts:
        addr, ln, kind = int(a, 16), int(n), int(k)
        assert addr % 4 == 0 and 1 <= ln <= 16 and addr + ln <= 0x1000
        assert kind != 0 and (kind != 7 or ln > 8)


# ------------------------------------------------------------------ #
# SystemVerilog DPI example: generate the problem package with        #
# packet_problem.py, build docs/examples/sv-dpi/packet_dpi.sv against  #
# dvs_dpi_pkg / dvs_randomizer_pkg and libdv_solve_dpi, run it, and    #
# check every printed packet against the constraints.                  #
# ------------------------------------------------------------------ #

def test_doc_sv_dpi_packet(example_env: dict, tmp_path: Path) -> None:
    import re
    import shutil
    vlt = _REPO / "packages" / "verilator-bin" / "bin" / "verilator"
    vlt = str(vlt) if vlt.is_file() else shutil.which("verilator")
    if vlt is None:
        pytest.skip("verilator not available")
    ex = _REPO / "docs" / "examples" / "sv-dpi"
    pkg = tmp_path / "packet_problem_pkg.sv"
    g = subprocess.run([sys.executable, str(ex / "packet_problem.py"), str(pkg)],
                       capture_output=True, text=True, env=example_env, timeout=60)
    assert g.returncode == 0, g.stdout + g.stderr
    q = subprocess.run(
        [sys.executable, "-c",
         "import dv_solve; print(dv_solve.get_svdirs()[0]); print(dv_solve.get_dpi_lib())"],
        capture_output=True, text=True, env=example_env, timeout=60)
    assert q.returncode == 0, q.stderr
    svdir, dpi = q.stdout.split()
    b = subprocess.run(
        [vlt, "--binary", "--top-module", "top", "--Mdir", str(tmp_path / "obj"), "-o", "sim",
         f"{svdir}/dvs_dpi_pkg.sv", f"{svdir}/dvs_randomizer_pkg.sv", str(pkg),
         str(ex / "packet_dpi.sv"), dpi, "-LDFLAGS", f"-Wl,-rpath,{os.path.dirname(dpi)}"],
        capture_output=True, text=True, timeout=900)
    assert b.returncode == 0, b.stdout[-2000:] + b.stderr[-2000:]
    r = subprocess.run([str(tmp_path / "obj" / "sim")], capture_output=True, text=True,
                       timeout=120)
    out = r.stdout + r.stderr
    assert r.returncode == 0, out
    pkts = re.findall(r"addr=([0-9a-f]+) len=(\d+) kind=(\d+)", out)
    assert len(pkts) == 5, out
    for a, n, k in pkts:
        addr, ln, kind = int(a, 16), int(n), int(k)
        assert addr % 4 == 0 and 1 <= ln <= 16 and addr + ln <= 0x1000
        assert kind != 0 and (kind != 7 or ln > 8)
    assert len(set(pkts)) > 1, out
    pinned = [int(n) for n in re.findall(r"pinned: len=(\d+)", out)]
    assert len(pinned) == 3 and all(8 < n <= 16 for n in pinned), out


# ------------------------------------------------------------------ #
# C example: compile docs/examples/c/packet.c against dv_solve.h with  #
# the directories the package reports, run it, check every packet.     #
# ------------------------------------------------------------------ #

def test_doc_c_packet(example_env: dict, tmp_path: Path) -> None:
    import re
    import shutil
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler")
    q = subprocess.run(
        [sys.executable, "-c",
         "import dv_solve as d; print(' '.join(['-I'+p for p in d.get_incdirs()]"
         " + ['-L'+p for p in d.get_libdirs()] + ['-Wl,-rpath,'+p for p in d.get_libdirs()]"
         " + ['-l'+l for l in d.get_libs()]))"],
        capture_output=True, text=True, env=example_env, timeout=60)
    assert q.returncode == 0, q.stderr
    exe = tmp_path / "packet"
    b = subprocess.run(
        [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
         str(_REPO / "docs" / "examples" / "c" / "packet.c"), "-o", str(exe)]
        + q.stdout.split(), capture_output=True, text=True, timeout=120)
    assert b.returncode == 0, b.stdout + b.stderr
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    pkts = re.findall(r"addr=([0-9a-f]+) len=(\d+) kind=(\d+)", r.stdout)
    assert len(pkts) == 5, r.stdout
    for a, n, k in pkts:
        addr, ln, kind = int(a, 16), int(n), int(k)
        assert addr % 4 == 0 and 1 <= ln <= 16 and addr + ln <= 0x1000
        assert kind != 0 and (kind != 7 or ln > 8)
    assert len(set(pkts)) > 1, r.stdout
