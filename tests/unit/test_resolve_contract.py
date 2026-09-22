"""The discovery contract in dv_solve._resolve.

Every case here builds a synthetic installation on disk and points the
resolver at it, because the property under test is "which of several
installations gets picked", and that is not observable with only one present.

The invariant these exist to protect: the ctypes loader and the public
link/compile helpers must always name the SAME installation. When they drift,
the Python API runs one build of the solver while generated C is linked
against another, and the ABI mismatch surfaces as a crash inside
``solver_compile`` with nothing pointing back at the cause.
"""
import os

import pytest

from dv_solve import _resolve


@pytest.fixture(autouse=True)
def _clean_env(monkeypatch):
    """Discovery must be decided by the test, not by the developer's shell."""
    monkeypatch.delenv("ZSP_SOLVER_PATH", raising=False)
    monkeypatch.delenv("LD_LIBRARY_PATH", raising=False)


def _lib(d, stem="dv_solve", suffix=""):
    """Create a plausible shared-library file and return its directory."""
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, _resolve.lib_filename(stem) + suffix), "wb").close()
    return d


def _headers(d):
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, _resolve._SENTINEL_HEADER), "w").close()
    return d


def _install(monkeypatch, pkg, root):
    """Point the resolver at a synthetic package dir and source root."""
    monkeypatch.setattr(_resolve, "_pkg_dir", lambda: str(pkg))
    monkeypatch.setattr(_resolve, "_src_root", lambda: str(root))


# ---------------------------------------------------------------- layouts --


def test_package_library_found(tmp_path, monkeypatch):
    pkg = tmp_path / "site" / "dv_solve"
    _install(monkeypatch, pkg, tmp_path / "checkout")
    _lib(str(pkg))
    assert _resolve.find_library("dv_solve") == str(
        pkg / _resolve.lib_filename("dv_solve"))


def test_package_lib_subdir_found(tmp_path, monkeypatch):
    """The package's own ``lib/`` -- honoured by the ctypes loader for as long
    as it has existed, but invisible to the public helpers until they shared
    this resolver."""
    pkg = tmp_path / "site" / "dv_solve"
    _install(monkeypatch, pkg, tmp_path / "checkout")
    os.makedirs(pkg, exist_ok=True)
    _lib(str(pkg / "lib"))
    assert _resolve.find_library("dv_solve") == str(
        pkg / "lib" / _resolve.lib_filename("dv_solve"))


@pytest.mark.parametrize("libdir", ["lib", "lib64", ""])
def test_cmake_libdir_layouts(tmp_path, monkeypatch, libdir):
    """CMake writes to ``lib`` or ``lib64`` depending on the distribution, and
    leaves the pre-install link output in the build root itself."""
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    os.makedirs(pkg, exist_ok=True)
    target = root / "build" / libdir if libdir else root / "build"
    _lib(str(target))
    assert _resolve.find_library("dv_solve") == str(
        target / _resolve.lib_filename("dv_solve"))


@pytest.mark.parametrize("name", _resolve._BUILD_DIR_NAMES)
def test_alternate_build_dir_names_still_supported(tmp_path, monkeypatch, name):
    """These four names were accepted by the ctypes loader before the
    resolvers were merged. Dropping any of them would silently break a
    checkout that uses it, so they are pinned."""
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    os.makedirs(pkg, exist_ok=True)
    _lib(str(root / name / "lib"))
    assert _resolve.find_library("dv_solve") is not None


def test_package_wins_over_build(tmp_path, monkeypatch):
    """An installed package is the answer even when a checkout build exists.

    This is the pre-existing precedence of BOTH resolvers and is deliberately
    preserved: a consumer that imported ``dv_solve`` from site-packages gets
    the libraries shipped with the thing it imported, not whatever happens to
    be lying in a neighbouring build tree.
    """
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    _lib(str(pkg))
    _lib(str(root / "build" / "lib"))
    assert os.path.dirname(_resolve.find_library("dv_solve")) == str(pkg)


def test_build_used_when_package_has_no_library(tmp_path, monkeypatch):
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    os.makedirs(pkg, exist_ok=True)
    _lib(str(root / "build" / "lib"))
    assert os.path.dirname(_resolve.find_library("dv_solve")) == str(
        root / "build" / "lib")


# --------------------------------------------------------------- override --


def test_explicit_override_beats_package(tmp_path, monkeypatch):
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    _lib(str(pkg))
    other = _lib(str(tmp_path / "elsewhere"))
    monkeypatch.setenv("ZSP_SOLVER_PATH", other)
    assert os.path.dirname(_resolve.find_library("dv_solve")) == other


def test_override_accepts_install_prefix(tmp_path, monkeypatch):
    """Pointing at a CMake install prefix, not just at a library directory."""
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    os.makedirs(pkg, exist_ok=True)
    prefix = tmp_path / "prefix"
    _lib(str(prefix / "lib"))
    _headers(str(prefix / "include"))
    monkeypatch.setenv("ZSP_SOLVER_PATH", str(prefix))
    assert os.path.dirname(_resolve.find_library("dv_solve")) == str(
        prefix / "lib")
    assert _resolve.find_incdirs() == [str(prefix / "include")]


def test_ld_library_path_is_a_fallback_not_an_override(tmp_path, monkeypatch):
    """Regression: an ambient LD_LIBRARY_PATH used to outrank the package in
    the ctypes loader while being invisible to ``get_libdirs()``, so the
    solver ran from one installation and generated C linked against another.
    Simulator wrapper scripts and CI images set this variable routinely, so
    the split happened without anyone opting into it.
    """
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    _lib(str(pkg))
    stray = _lib(str(tmp_path / "stray"))
    monkeypatch.setenv("LD_LIBRARY_PATH", stray)
    assert os.path.dirname(_resolve.find_library("dv_solve")) == str(pkg)


def test_ld_library_path_still_resolves_when_nothing_else_does(
        tmp_path, monkeypatch):
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    os.makedirs(pkg, exist_ok=True)
    stray = _lib(str(tmp_path / "stray"))
    monkeypatch.setenv("LD_LIBRARY_PATH", stray)
    assert os.path.dirname(_resolve.find_library("dv_solve")) == stray


# ------------------------------------------------------- missing artifacts --


def test_missing_library_returns_none(tmp_path, monkeypatch):
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    os.makedirs(pkg, exist_ok=True)
    assert _resolve.find_library("dv_solve") is None


def test_existing_but_empty_include_dir_is_not_accepted(tmp_path, monkeypatch):
    """A staging bug that creates ``share/include`` and populates nothing used
    to pass an ``isdir`` test and defer the failure to a consumer's compile."""
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    os.makedirs(pkg / "share" / "include" / "dv_solve")
    assert _resolve.find_incdirs() is None


def test_installed_include_set_has_nested_dir(tmp_path, monkeypatch):
    """Unqualified includes (``#include "zsp_ctx.h"``) only resolve against the
    nested ``dv_solve/`` directory; the base holds no headers at all. pssc's
    generated ``pssc_solve.c`` emits exactly those unqualified includes."""
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    base = pkg / "share" / "include"
    _headers(str(base / "dv_solve"))
    got = _resolve.find_incdirs()
    assert str(base / "dv_solve") in got
    assert str(base) in got


def test_source_tree_headers_are_flat(tmp_path, monkeypatch):
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    os.makedirs(pkg, exist_ok=True)
    _headers(str(root / "src" / "c"))
    assert _resolve.find_incdirs() == [str(root / "src" / "c")]


# ------------------------------------------------------- versioned sonames --


def test_versioned_soname_loadable_but_not_linkable(tmp_path, monkeypatch):
    """``libdv_solve.so.1`` can be dlopen()ed but ``-ldv_solve`` will not find
    it. The loader and the linker therefore have to apply different acceptance
    rules to stay on the same installation -- reporting a versioned-only
    directory from ``get_libdirs()`` would fail at the end of a long build
    with ``cannot find -ldv_solve``.
    """
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    os.makedirs(pkg, exist_ok=True)
    open(pkg / (_resolve.lib_filename("dv_solve") + ".1"), "wb").close()
    assert _resolve.find_library("dv_solve") is not None
    assert _resolve.find_library("dv_solve", linkable=True) is None


def test_unversioned_preferred_over_versioned(tmp_path, monkeypatch):
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    _lib(str(pkg))
    open(pkg / (_resolve.lib_filename("dv_solve") + ".1"), "wb").close()
    assert _resolve.find_library("dv_solve").endswith(
        _resolve.lib_filename("dv_solve"))


# ------------------------------------------------------ independent stems --


def test_core_and_dpi_resolve_independently(tmp_path, monkeypatch):
    """``get_dpi_lib()`` must probe for its OWN filename rather than appending
    to whatever directory the core library came from: a ``-sv_lib`` pointed at
    a path that merely ought to exist fails inside the simulator's elaborator,
    a long way from anything naming dv-solve."""
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    _lib(str(pkg), stem="dv_solve")
    assert _resolve.find_library("dv_solve") is not None
    assert _resolve.find_library("dv_solve_dpi") is None


# ------------------------------------------------------------- no /tmp use --


def test_tmp_pytest_builds_are_not_searched(tmp_path, monkeypatch):
    """The loader used to glob ``/tmp/pytest-*/zsp_build*``, letting an
    unrelated and possibly half-built test tree supply the solver to
    production code on any box that had ever run the suite."""
    pkg, root = tmp_path / "site" / "dv_solve", tmp_path / "checkout"
    _install(monkeypatch, pkg, root)
    os.makedirs(pkg, exist_ok=True)
    # Plant a library exactly where the old glob would have picked it up.
    # (Asserting on the search-dir strings would not work: pytest's own
    # tmp_path lives under /tmp/pytest-*, so the synthetic package dir matches
    # that pattern too. What matters is that the glob no longer runs.)
    _lib(str(tmp_path.parent / "zsp_build"))
    assert _resolve.find_library("dv_solve") is None


# ---------------------------------------------------- loader/linker parity --


def test_loader_and_link_dirs_agree_on_the_real_installation():
    """The end-to-end invariant, against whatever is actually installed here.

    Skipped rather than failed when no library is present, so the contract
    tests above still run on a host with no build.
    """
    import dv_solve
    from dv_solve import lib as _lib_mod

    loaded = _lib_mod._find_library()
    if loaded is None:
        pytest.skip("no dv-solve library on this host")
    assert os.path.dirname(str(loaded)) == dv_solve.get_libdirs()[0]
