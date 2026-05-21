"""Benchmark conftest — marker registration and solver registry export."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

# Make the bench directory importable so test files can do `from solvers import solvers`.
_BENCH_DIR = str(Path(__file__).parent)
if _BENCH_DIR not in sys.path:
    sys.path.insert(0, _BENCH_DIR)

_PKG_DIR = Path(__file__).parent.parent.parent  # packages/zuspec-solver

from solvers import solvers  # noqa: F401  (re-exported for test files)


def pytest_addoption(parser):
    parser.addoption(
        "--bench-target-secs", type=float, default=None,
        help="Target wall-clock seconds per benchmark (default: 5.0)",
    )
    parser.addoption(
        "--bench-timeout-secs", type=float, default=None,
        help="Hard timeout seconds per benchmark (default: 10.0)",
    )


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "bench: throughput benchmark (use -m bench to select)",
    )
    # Push CLI overrides into env vars so solvers pick them up via get_bench_config().
    target = config.getoption("--bench-target-secs", default=None)
    timeout = config.getoption("--bench-timeout-secs", default=None)
    if target is not None:
        os.environ["ZSP_BENCH_TARGET"] = str(target)
    if timeout is not None:
        os.environ["ZSP_BENCH_TIMEOUT"] = str(timeout)


def _build_native_lib(build_dir: Path) -> Path:
    """Build libdv_solve.so into build_dir via CMake."""
    build_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cmake", str(_PKG_DIR), "-DCMAKE_BUILD_TYPE=Release"],
        cwd=build_dir, check=True, capture_output=True,
    )
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--parallel"],
        check=True, capture_output=True,
    )
    hits = sorted(build_dir.glob("libdv_solve.so*"), key=lambda p: len(p.name))
    if not hits:
        raise FileNotFoundError("libdv_solve.so not found after build")
    return hits[0]


@pytest.fixture(scope="session", autouse=True)
def _native_lib_path(tmp_path_factory):
    """Build the native solver library once per session and set ZSP_SOLVER_PATH."""
    if not shutil.which("cmake"):
        return None
    # Check if library is already available (e.g. from a prior build or env)
    import dv_solve.lib as _lib_mod
    if _lib_mod._load_lib() is not None:
        return None  # already found; no need to rebuild

    build_dir = tmp_path_factory.mktemp("zsp_bench_build")
    try:
        lib_path = _build_native_lib(build_dir)
        # Reset cache so the new path is picked up
        _lib_mod._LOAD_ATTEMPTED = False
        _lib_mod._LIB_CACHE = None
        os.environ["ZSP_SOLVER_PATH"] = str(lib_path.parent)
        _lib_mod._load_lib()  # warm the cache
        return lib_path
    except Exception:
        return None
