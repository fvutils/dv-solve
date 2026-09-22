"""The single source of truth for locating dv-solve's native artifacts.

Both consumers of this module must agree, or a build silently mixes two
installations:

  * ``lib.py`` dlopen()s the core library for the Python API;
  * ``__init__.py``'s ``get_libdirs`` / ``get_dpi_lib`` / ``get_incdirs`` /
    ``get_svdirs`` tell a C consumer what to compile and link against.

Those two used to search different places in a different order -- the loader
consulted ``LD_LIBRARY_PATH``, the package's ``lib/`` subdirectory, three
alternate build-directory names and a ``/tmp`` pytest glob that the public
helpers knew nothing about. A host with any of those set could therefore run
the solver out of one installation while linking generated C against another,
and the ABI mismatch surfaces as a crash inside ``solver_compile``, nowhere
near its cause. Hence one resolver, used by both.

THE CONTRACT
------------

Search order for every artifact (library, header, SV source):

  1. ``ZSP_SOLVER_PATH`` -- the explicit override. Highest precedence, always.
     Accepts either a directory holding the artifacts or one with the usual
     ``lib/``, ``lib64/``, ``include/``, ``share/`` subdirectories.
  2. The installed package: the directory holding ``__init__.py``, then its
     ``lib/`` subdirectory. This is the binary-wheel layout.
  3. A built source checkout: ``<root>/build`` and its ``lib``/``lib64``
     install destinations, for each supported build-directory name.
  4. ``LD_LIBRARY_PATH`` -- a last-resort fallback, NOT an override.

Rationale for (4) being last. It used to come second, ahead of the package,
which meant an ambient setting -- a simulator's wrapper script, a module-load,
a CI runner's default -- silently redirected the loader while the public
helpers, which never consulted it at all, kept reporting the package. That is
precisely the loader/linker split this module exists to prevent. ``LD_LIBRARY_
PATH`` still resolves the *dependencies* of whatever library is chosen; it no
longer chooses. Callers who genuinely want to select an out-of-tree build have
``ZSP_SOLVER_PATH``, which is unambiguous about intent.

Everything here probes for the ARTIFACT, never for the directory. A configured
but unbuilt CMake tree has a ``build/lib`` the moment it is configured, and an
unbuilt source checkout has a ``src/c`` full of headers but no library; a
directory existing says nothing about whether the thing you need is in it.
"""
from __future__ import annotations

import os
import platform
from typing import List, Optional

#: Build-directory names a checkout may use. ``build`` is what this project's
#: own instructions and CI produce; the rest are long-supported aliases that
#: the ctypes loader has always accepted, kept so that dropping them is not a
#: silent regression for anyone whose tree uses one.
_BUILD_DIR_NAMES = ("build", "_build", "build_release", "cmake-build-release")

#: Where CMake puts libraries under a build directory. The empty string is the
#: pre-install link output that sits in the build root itself, which makes a
#: checkout that has been built but never ``cmake --install``-ed resolve.
_LIB_SUBDIRS = ("lib", "lib64", "")

#: A header that exists in every dv-solve include tree, used to tell a real
#: include directory from a directory that merely exists.
_SENTINEL_HEADER = "zsp_problem.h"

#: Likewise for the SystemVerilog resource directory.
_SENTINEL_SV = "zsp_dpi_pkg.sv"


def _pkg_dir() -> str:
    return os.path.dirname(os.path.abspath(__file__))


def _src_root() -> str:
    """``packages/dv-solve/`` when imported from a source tree (or editable
    install). Meaningless for a wheel installed into site-packages, where it
    points at some interpreter-internal directory -- harmless, because every
    path built from it is then probed for an artifact that will not be there.
    """
    return os.path.abspath(os.path.join(_pkg_dir(), "..", ".."))


def lib_filename(stem: str) -> str:
    """The platform's file name for shared library *stem* (no directory)."""
    system = platform.system()
    if system == "Windows":
        return "%s.dll" % stem
    if system == "Darwin":
        return "lib%s.dylib" % stem
    return "lib%s.so" % stem


def lib_patterns(stem: str) -> List[str]:
    """Glob patterns for library *stem*, most-preferred first.

    The unversioned name comes first because it is the only one a linker will
    accept for ``-l<stem>``; the soname-suffixed form is loadable by dlopen but
    not linkable, so preferring it would let the loader and the linker pick
    different files.
    """
    system = platform.system()
    if system == "Windows":
        return ["%s.dll" % stem, "lib%s.dll" % stem]
    if system == "Darwin":
        return ["lib%s.dylib" % stem, "lib%s.*.dylib" % stem]
    return ["lib%s.so" % stem, "lib%s.so.*" % stem]


def _override_dirs() -> List[str]:
    """Directories contributed by ``ZSP_SOLVER_PATH``.

    The variable has historically pointed straight at a directory containing
    the library, so that stays first. A prefix-style value (one with ``lib/``,
    ``include/`` or ``share/`` underneath) also works, because a user who
    points at a CMake install prefix reasonably expects that to be understood.
    """
    root = os.environ.get("ZSP_SOLVER_PATH")
    if not root:
        return []
    dirs = [root]
    for sub in ("lib", "lib64", "include", os.path.join("share", "include"),
                os.path.join("include", "dv_solve"),
                os.path.join("share", "include", "dv_solve"),
                os.path.join("share", "sv"), "sv"):
        dirs.append(os.path.join(root, sub))
    return dirs


def _ld_library_path_dirs() -> List[str]:
    var = "PATH" if platform.system() == "Windows" else "LD_LIBRARY_PATH"
    return [d for d in os.environ.get(var, "").split(os.pathsep) if d]


def _build_dirs() -> List[str]:
    root = _src_root()
    out = []
    for name in _BUILD_DIR_NAMES:
        for sub in _LIB_SUBDIRS:
            out.append(os.path.join(root, name, sub) if sub
                       else os.path.join(root, name))
    return out


def lib_search_dirs() -> List[str]:
    """Every directory that may hold a dv-solve shared library, best first.

    See the module docstring for why the order is what it is.
    """
    dirs = list(_override_dirs())
    dirs.append(_pkg_dir())
    dirs.append(os.path.join(_pkg_dir(), "lib"))
    dirs.extend(_build_dirs())
    dirs.extend(_ld_library_path_dirs())          # fallback, never an override
    return dirs


def find_library(stem: str, linkable: bool = False) -> Optional[str]:
    """Absolute path to shared library *stem*, or ``None``.

    With ``linkable=True`` only the unversioned name is accepted, because that
    is what ``-l<stem>`` resolves; a directory holding just ``libfoo.so.1`` can
    be dlopen()ed but cannot be linked against, and reporting it from
    ``get_libdirs()`` would produce a ``cannot find -ldv_solve`` at the very
    end of a long build.
    """
    import glob

    patterns = lib_patterns(stem)[:1] if linkable else lib_patterns(stem)
    for d in lib_search_dirs():
        if not os.path.isdir(d):
            continue
        for pattern in patterns:
            hits = sorted(glob.glob(os.path.join(d, pattern)),
                          key=lambda p: len(os.path.basename(p)))
            if hits:
                return hits[0]
    return None


def _inc_candidates() -> List[List[str]]:
    """Candidate include-directory SETS, best first.

    A set rather than a single directory because the installed layout needs two
    entries: headers are staged under ``share/include/dv_solve/`` (CMake's
    ``DESTINATION include/dv_solve``), while dv-solve's own sources and pssc's
    generated ``pssc_solve.c`` both use UNQUALIFIED includes (``#include
    "zsp_ctx.h"``), which only resolve against the nested directory. The base
    is kept alongside it for any consumer that writes ``dv_solve/zsp_ctx.h``.

    NOTE ON THE COLLISION: dv-solve and zuspec-be-sw both ship a
    ``zsp_alloc.h`` and define ``struct zsp_alloc_s`` incompatibly. These
    directories are the include set for the SOLVER translation unit ONLY, and
    must never be merged into one ``-I`` list with the backend's. Namespacing
    the install under ``dv_solve/`` does not by itself protect against this,
    because the nested directory has to be on the include path for unqualified
    includes to work -- it is the per-TU segregation that keeps them apart.
    """
    pkg, root = _pkg_dir(), _src_root()
    sets = []
    for d in _override_dirs():
        sets.append([d])
    for base in (os.path.join(pkg, "share", "include"),
                 os.path.join(pkg, "include")):
        sets.append([base, os.path.join(base, "dv_solve")])
    sets.append([os.path.join(root, "src", "c")])          # source tree: flat
    for name in _BUILD_DIR_NAMES:
        base = os.path.join(root, name, "include")
        sets.append([base, os.path.join(base, "dv_solve")])
    return sets


def find_incdirs() -> Optional[List[str]]:
    """The include set for compiling against dv-solve, or ``None``.

    Validated by probing for a real header: an unbuilt checkout and a wheel
    whose data files failed to stage both leave a plausible-looking directory
    behind, and handing that to ``gcc -I`` defers the failure to a confusing
    ``zsp_block_alloc.h: No such file or directory`` in generated code.
    """
    for candidate in _inc_candidates():
        for d in candidate:
            if os.path.isfile(os.path.join(d, _SENTINEL_HEADER)):
                return [p for p in candidate if os.path.isdir(p)]
    return None


def find_svdirs() -> Optional[List[str]]:
    """The SystemVerilog package search path, or ``None``."""
    pkg, root = _pkg_dir(), _src_root()
    candidates = list(_override_dirs())
    candidates += [os.path.join(pkg, "share", "sv"),
                   os.path.join(root, "src", "sv")]
    for name in _BUILD_DIR_NAMES:
        candidates.append(os.path.join(root, name, "share", "sv"))
    for d in candidates:
        if os.path.isfile(os.path.join(d, _SENTINEL_SV)):
            return [d]
    return None


def describe_search() -> str:
    """A rendered search path, for embedding in diagnostics."""
    return "\n".join("  - %s" % d for d in lib_search_dirs())


def missing_artifact_error(what: str, detail: str = "") -> RuntimeError:
    """A uniform, actionable error for any missing native artifact."""
    return RuntimeError(
        "dv-solve: %s not found.%s\n"
        "Searched:\n%s\n"
        "Fixes:\n"
        "  - Install a binary wheel:  pip install dv-solve\n"
        "  - Build from source (needs CMake + a C compiler):\n"
        "        cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=build\n"
        "        ninja -C build install\n"
        "  - Or point ZSP_SOLVER_PATH at the install prefix / directory."
        % (what, (" " + detail) if detail else "", describe_search()))
