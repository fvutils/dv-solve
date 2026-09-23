"""The discovery helpers a C/SV consumer compiles and links against.

These resolve differently in an installed wheel (native libraries staged at the
package root) than in a source tree (CMake writes them to ``build/<libdir>``),
and returning the wheel answer unconditionally is a defect a consumer only sees
as a *build* failure in someone else's project -- ``cannot find -ldv_solve``, or
``fatal error: zsp_block_alloc.h`` before that. Hence a test per layout, with the
layout faked so neither depends on how this checkout happens to be built.
"""
import os

import pytest

import dv_solve
from dv_solve import _resolve


@pytest.fixture
def layout(tmp_path, monkeypatch):
    """Fake both layouts side by side and let the test populate either.

    Returns ``(pkg_dir, src_root)``: patching the two private accessors is what
    makes the precedence between them testable at all, since the real ones are
    derived from ``__file__``.

    They now live in ``dv_solve._resolve``, the single search implementation
    shared with the ctypes loader -- patching them on ``dv_solve`` itself would
    no longer redirect anything. The environment is cleared for the same
    reason: that shared resolver honours ``ZSP_SOLVER_PATH`` and (as a last
    resort) ``LD_LIBRARY_PATH``, so a developer's ambient settings would
    otherwise decide the result of a test about layout precedence.
    """
    pkg_dir = tmp_path / "site-packages" / "dv_solve"
    src_root = tmp_path / "checkout"
    pkg_dir.mkdir(parents=True)
    src_root.mkdir()
    monkeypatch.delenv("ZSP_SOLVER_PATH", raising=False)
    monkeypatch.delenv("LD_LIBRARY_PATH", raising=False)
    monkeypatch.setattr(_resolve, "_pkg_dir", lambda: str(pkg_dir))
    monkeypatch.setattr(_resolve, "_src_root", lambda: str(src_root))
    return pkg_dir, src_root


def _touch(directory, name):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / name).write_bytes(b"")


def test_libdirs_finds_the_wheel_layout(layout):
    """A staged library next to ``__init__.py`` is the installed-wheel answer."""
    pkg_dir, _ = layout
    _touch(pkg_dir, dv_solve._lib_filename("dv_solve"))
    assert dv_solve.get_libdirs() == [str(pkg_dir)]


def test_libdirs_finds_a_source_tree_build(layout):
    """``build/lib`` is where CMake installs, and where the wheel stages from."""
    _pkg_dir, src_root = layout
    _touch(src_root / "build" / "lib", dv_solve._lib_filename("dv_solve"))
    assert dv_solve.get_libdirs() == [str(src_root / "build" / "lib")]


def test_libdirs_finds_a_lib64_source_tree(layout):
    """Some distributions' CMake uses ``lib64``; the wheel's ``{libdir}``
    template covers both, so this must too."""
    _pkg_dir, src_root = layout
    _touch(src_root / "build" / "lib64", dv_solve._lib_filename("dv_solve"))
    assert dv_solve.get_libdirs() == [str(src_root / "build" / "lib64")]


def test_libdirs_finds_a_built_but_uninstalled_tree(layout):
    """``cmake --build`` without ``cmake --install`` leaves the library in
    ``build/`` only. That is a normal state mid-development, not an error."""
    _pkg_dir, src_root = layout
    _touch(src_root / "build", dv_solve._lib_filename("dv_solve"))
    assert dv_solve.get_libdirs() == [str(src_root / "build")]


def test_libdirs_prefers_the_wheel_to_a_stale_build_tree(layout):
    """With both present the installed library wins: an editable install is the
    only case where a checkout should be consulted, and there the package
    directory holds no library at all."""
    pkg_dir, src_root = layout
    name = dv_solve._lib_filename("dv_solve")
    _touch(pkg_dir, name)
    _touch(src_root / "build" / "lib", name)
    assert dv_solve.get_libdirs() == [str(pkg_dir)]


def test_libdirs_does_not_report_a_configured_but_unlinked_build_dir(layout):
    """CMake creates ``build/lib`` when it *configures*, long before anything is
    linked into it -- so probing for the directory rather than the library would
    hand a consumer a path with no library in it. Falls back to the package
    directory, keeping the failure the same as a broken wheel's."""
    pkg_dir, src_root = layout
    (src_root / "build" / "lib").mkdir(parents=True)
    assert dv_solve.get_libdirs() == [str(pkg_dir)]


def test_dpi_lib_follows_the_same_search(layout):
    """The DPI library is staged and built alongside the main one, so it shared
    the source-tree gap. It comes from the installation the core library
    does, so the build that holds it must hold the core library too."""
    _pkg_dir, src_root = layout
    name = dv_solve._lib_filename("dv_solve_dpi")
    _touch(src_root / "build" / "lib", dv_solve._lib_filename("dv_solve"))
    _touch(src_root / "build" / "lib", name)
    assert dv_solve.get_dpi_lib() == str(src_root / "build" / "lib" / name)


# --- against this checkout, whatever layout it happens to have --------------

def test_the_reported_libdir_actually_holds_the_library():
    """The whole point of the helper: what it names must be linkable.

    Skipped rather than failed when nothing is built -- that is a statement about
    the checkout, not about the code under test.
    """
    libdir = dv_solve.get_libdirs()[0]
    lib = os.path.join(libdir, dv_solve._lib_filename("dv_solve"))
    if not os.path.isfile(lib):
        pytest.skip("dv-solve is not built in this checkout: no %s" % lib)
    assert os.path.isfile(lib)


def test_the_reported_incdirs_actually_hold_the_headers():
    """The companion check for headers, which had this same defect first:
    an unqualified `#include "zsp_ctx.h"` has to resolve against one of them."""
    incdirs = dv_solve.get_incdirs()
    assert incdirs
    assert any(os.path.isfile(os.path.join(d, "zsp_ctx.h")) for d in incdirs), \
        "no reported include dir holds zsp_ctx.h: %s" % incdirs
