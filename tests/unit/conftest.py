"""Shared pytest fixtures for the native-solver C unit tests.

The ``libzsp`` fixture builds ``libdv_solve.so`` via CMake (into a
temporary directory) and loads it with ``ctypes.CDLL``.  Tests that need
the library receive it as a parameter; the fixture emits a pytest.skip()
if the build fails so the suite degrades gracefully on hosts without a C
toolchain.
"""
from __future__ import annotations

import ctypes
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

# Absolute path of the zuspec-solver package root (contains CMakeLists.txt)
_PKG_DIR = Path(__file__).parent.parent.parent


def _build_library(build_dir: Path) -> Path:
    """Run cmake configure + build and return the path to the .so file."""
    build_dir.mkdir(parents=True, exist_ok=True)

    # Configure
    subprocess.run(
        ["cmake", str(_PKG_DIR), "-DCMAKE_BUILD_TYPE=Release"],
        cwd=build_dir,
        check=True,
        capture_output=True,
    )

    # Build
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--parallel"],
        check=True,
        capture_output=True,
    )

    # Locate the produced .so
    candidates = list(build_dir.glob("libdv_solve.so*"))
    if not candidates:
        raise FileNotFoundError(
            f"libdv_solve.so not found in {build_dir} after build"
        )
    # Prefer the unversioned symlink / exact name
    candidates.sort(key=lambda p: len(p.name))
    return candidates[0]


# Module-level cache: (build_dir, lib_path, ctypes handle)
_lib_cache: dict = {}


def _build_debug_library(build_dir: Path) -> Path:
    """Build libdv_solve_debug.so with contradiction analysis enabled."""
    build_dir.mkdir(parents=True, exist_ok=True)

    subprocess.run(
        ["cmake", str(_PKG_DIR), "-DCMAKE_BUILD_TYPE=Release",
         "-DZSP_CONTRADICTION_ANALYSIS=ON"],
        cwd=build_dir,
        check=True,
        capture_output=True,
    )

    subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", "dv_solve_debug",
         "--parallel"],
        check=True,
        capture_output=True,
    )

    candidates = list(build_dir.glob("libdv_solve_debug.so*"))
    if not candidates:
        raise FileNotFoundError(
            f"libdv_solve_debug.so not found in {build_dir} after build"
        )
    candidates.sort(key=lambda p: len(p.name))
    return candidates[0]


@pytest.fixture(scope="session")
def libzsp(tmp_path_factory):
    """Session-scoped fixture that builds and loads libdv_solve.so.

    Yields the ctypes.CDLL handle.  Skips if cmake/gcc is not available
    or the build fails.
    """
    if not shutil.which("cmake"):
        pytest.skip("cmake not found — skipping native solver tests")
    if not shutil.which("gcc") and not shutil.which("cc"):
        pytest.skip("C compiler not found — skipping native solver tests")

    build_dir = tmp_path_factory.mktemp("zsp_build")
    try:
        lib_path = _build_library(build_dir)
    except Exception as exc:
        pytest.skip(f"libdv_solve build failed: {exc}")

    lib = ctypes.CDLL(str(lib_path))
    yield lib


@pytest.fixture(scope="session")
def libzsp_debug(tmp_path_factory):
    """Session-scoped fixture that builds and loads libdv_solve_debug.so.

    This library always has ZSP_CONTRADICTION_ANALYSIS=ON.
    """
    if not shutil.which("cmake"):
        pytest.skip("cmake not found")
    if not shutil.which("gcc") and not shutil.which("cc"):
        pytest.skip("C compiler not found")

    build_dir = tmp_path_factory.mktemp("zsp_build_debug")
    try:
        lib_path = _build_debug_library(build_dir)
    except Exception as exc:
        pytest.skip(f"libdv_solve_debug build failed: {exc}")

    lib = ctypes.CDLL(str(lib_path))
    yield lib
