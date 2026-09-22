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

The unit of discovery is an INSTALLATION, not an artifact. An installation is
one root together with the places its libraries, headers and SV sources live.
Candidates, best first:

  1. ``ZSP_SOLVER_PATH`` -- the explicit override. When set it is the ONLY
     candidate: it is selected whether or not it is usable, and anything it
     lacks is reported as missing from it rather than supplied by a later
     candidate. Accepts a directory holding the artifacts or an install
     prefix with the usual ``lib/``, ``lib64/``, ``include/``, ``share/``
     subdirectories.
  2. The installed package: the directory holding ``__init__.py``, then its
     ``lib/`` subdirectory. This is the binary-wheel layout.
  3. A built source checkout, one candidate per supported build-directory
     name: ``<root>/<build>`` and its ``lib``/``lib64`` install destinations.
  4. Each ``LD_LIBRARY_PATH`` entry -- a last-resort, library-only fallback,
     NOT an override. It supplies no headers or SV sources.

SELECTION: the override if set, otherwise the first candidate holding a
loadable core library (``libdv_solve.so`` or a numerically versioned
``libdv_solve.so.N...``). Every artifact -- core library for the loader, core
library for the linker, DPI library, headers, SV sources -- is then taken
from the selected installation and from nowhere else. An artifact the selected
installation lacks is MISSING; it is never borrowed from another candidate,
because that is exactly how a build mixes two solver builds.

LOADING VS LINKING within the selected installation: the unversioned name is
preferred for both, so whenever the installation is linkable the loader opens
the very file ``-ldv_solve`` resolves. An installation holding only a
versioned library is loadable but NOT linkable; ``require_library(...,
linkable=True)`` then raises naming that installation, rather than letting the
linker fall through to a different one.

When no candidate holds a core library at all, nothing is selected. Headers
and SV sources may still be located (from the first candidate that has them)
for compile-only consumers; there is no library for them to disagree with.

Rationale for (4) being last. It used to come second, ahead of the package,
which meant an ambient setting -- a simulator's wrapper script, a module-load,
a CI runner's default -- silently redirected the loader while the public
helpers, which never consulted it at all, kept reporting the package. That is
precisely the loader/linker split this module exists to prevent. ``LD_LIBRARY_
PATH`` still resolves the *dependencies* of whatever library is chosen; it
chooses only when nothing else can. Callers who genuinely want to select an
out-of-tree build have ``ZSP_SOLVER_PATH``, which is unambiguous about intent.

Everything here probes for the ARTIFACT, never for the directory, and an
artifact must be a real file: a directory named ``libdv_solve.so`` or a
dangling symlink is not a library (a valid symlink to a library is). A
configured but unbuilt CMake tree has a ``build/lib`` the moment it is
configured, and an unbuilt source checkout has a ``src/c`` full of headers
but no library; a directory existing says nothing about whether the thing you
need is in it.
"""
from __future__ import annotations

import os
import platform
import re
from typing import List, NamedTuple, Optional

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


class Installation(NamedTuple):
    """One candidate root and where its artifacts may live.

    ``inc_sets`` is a list of include-directory SETS rather than directories:
    the installed layout needs two entries (see :func:`find_incdirs`).
    """
    kind: str               # "override" | "package" | "checkout" | "ld_library_path"
    root: str
    lib_dirs: List[str]
    inc_sets: List[List[str]]
    sv_dirs: List[str]

    def describe(self) -> str:
        if self.kind == "override":
            return "ZSP_SOLVER_PATH=%s" % self.root
        if self.kind == "ld_library_path":
            return "LD_LIBRARY_PATH entry %s" % self.root
        return "%s installation at %s" % (self.kind, self.root)


def _override() -> Optional[Installation]:
    """The ``ZSP_SOLVER_PATH`` installation, or ``None`` when unset.

    The variable has historically pointed straight at a directory containing
    the library, so that stays first. A prefix-style value (one with ``lib/``,
    ``include/`` or ``share/`` underneath) also works, because a user who
    points at a CMake install prefix reasonably expects that to be understood.
    """
    root = os.environ.get("ZSP_SOLVER_PATH")
    if not root:
        return None
    j = os.path.join
    inc_sets = [[root]]
    for base in (j(root, "include"), j(root, "share", "include")):
        inc_sets.append([base, j(base, "dv_solve")])
    return Installation(
        "override", root,
        lib_dirs=[root, j(root, "lib"), j(root, "lib64")],
        inc_sets=inc_sets,
        sv_dirs=[root, j(root, "share", "sv"), j(root, "sv")])


def _package() -> Installation:
    pkg, j = _pkg_dir(), os.path.join
    return Installation(
        "package", pkg,
        lib_dirs=[pkg, j(pkg, "lib")],
        inc_sets=[[b, j(b, "dv_solve")]
                  for b in (j(pkg, "share", "include"), j(pkg, "include"))],
        sv_dirs=[j(pkg, "share", "sv")])


def _checkouts() -> List[Installation]:
    """One candidate per build directory of a source checkout.

    The checkout's flat ``src/c`` headers and ``src/sv`` sources belong to
    every build of it; a build's own ``include``/``share/sv`` install tree
    follows as an alternative.
    """
    root, j = _src_root(), os.path.join
    out = []
    for name in _BUILD_DIR_NAMES:
        bd = j(root, name)
        out.append(Installation(
            "checkout", bd,
            lib_dirs=[j(bd, sub) if sub else bd for sub in _LIB_SUBDIRS],
            inc_sets=[[j(root, "src", "c")],
                      [j(bd, "include"), j(bd, "include", "dv_solve")]],
            sv_dirs=[j(root, "src", "sv"), j(bd, "share", "sv")]))
    return out


def _ld_library_path_dirs() -> List[str]:
    var = "PATH" if platform.system() == "Windows" else "LD_LIBRARY_PATH"
    return [d for d in os.environ.get(var, "").split(os.pathsep) if d]


def candidates() -> List[Installation]:
    """Every candidate installation, best first. See the module docstring."""
    override = _override()
    if override is not None:
        return [override]                       # terminal: nothing else competes
    out = [_package()] + _checkouts()
    out += [Installation("ld_library_path", d, [d], [], [])
            for d in _ld_library_path_dirs()]   # fallback, never an override
    return out


def lib_search_dirs() -> List[str]:
    """Every directory that may hold a dv-solve shared library, best first."""
    return [d for inst in candidates() for d in inst.lib_dirs]


# ------------------------------------------------------------ file probing --


def _versioned_re(stem: str) -> Optional["re.Pattern[str]"]:
    """Names of a numerically versioned library -- loadable, not linkable.

    Numeric suffixes only, so ``libdv_solve.so.1.debug`` or a stray
    ``libdv_solve.so.bak`` is never mistaken for the library.
    """
    system = platform.system()
    if system == "Windows":
        return None
    if system == "Darwin":
        return re.compile(r"^lib%s(\.\d+)+\.dylib$" % re.escape(stem))
    return re.compile(r"^lib%s\.so(\.\d+)+$" % re.escape(stem))


def _is_file(path: str) -> bool:
    """A real file, or a symlink that ends at one. ``isfile`` follows links,
    so this rejects both directories and dangling symlinks."""
    return os.path.isfile(path)


def _unversioned_in(d: str, stem: str) -> Optional[str]:
    p = os.path.join(d, lib_filename(stem))
    return p if _is_file(p) else None


def _versioned_in(d: str, stem: str) -> Optional[str]:
    rx = _versioned_re(stem)
    if rx is None or not os.path.isdir(d):
        return None
    hits = sorted((n for n in os.listdir(d) if rx.match(n)
                   and _is_file(os.path.join(d, n))), key=lambda n: (len(n), n))
    return os.path.join(d, hits[0]) if hits else None


def library_in(inst: Installation, stem: str,
               linkable: bool = False) -> Optional[str]:
    """Library *stem* within *inst* only, or ``None``.

    The unversioned name is sought across ALL of the installation's library
    directories before any versioned name is considered, so that whenever the
    installation can be linked, the loader opens the same file the linker
    uses.
    """
    for d in inst.lib_dirs:
        hit = _unversioned_in(d, stem)
        if hit:
            return hit
    if linkable:
        return None
    for d in inst.lib_dirs:
        hit = _versioned_in(d, stem)
        if hit:
            return hit
    return None


def _incdirs_in(inst: Installation) -> Optional[List[str]]:
    for candidate in inst.inc_sets:
        for d in candidate:
            if _is_file(os.path.join(d, _SENTINEL_HEADER)):
                return [p for p in candidate if os.path.isdir(p)]
    return None


def _svdirs_in(inst: Installation) -> Optional[List[str]]:
    for d in inst.sv_dirs:
        if _is_file(os.path.join(d, _SENTINEL_SV)):
            return [d]
    return None


# --------------------------------------------------------------- selection --


def select_installation() -> Optional[Installation]:
    """The installation every artifact comes from, or ``None`` if none.

    The override when set (usable or not); otherwise the first candidate
    holding a loadable core library.
    """
    cands = candidates()
    if cands and cands[0].kind == "override":
        return cands[0]
    for inst in cands:
        if library_in(inst, "dv_solve") is not None:
            return inst
    return None


def find_library(stem: str, linkable: bool = False) -> Optional[str]:
    """Absolute path to shared library *stem* in the selected installation,
    or ``None``.

    With ``linkable=True`` only the unversioned name is accepted, because that
    is what ``-l<stem>`` resolves. ``None`` then means the SELECTED
    installation cannot be linked -- a later candidate is never consulted
    instead. Use :func:`require_library` for the diagnostic.
    """
    inst = select_installation()
    return library_in(inst, stem, linkable) if inst is not None else None


def find_incdirs() -> Optional[List[str]]:
    """The include set for compiling against dv-solve, or ``None``.

    A set rather than a single directory because the installed layout needs
    two entries: headers are staged under ``share/include/dv_solve/``
    (CMake's ``DESTINATION include/dv_solve``), while dv-solve's own sources
    and pssc's generated ``pssc_solve.c`` both use UNQUALIFIED includes
    (``#include "zsp_ctx.h"``), which only resolve against the nested
    directory. The base is kept alongside it for any consumer that writes
    ``dv_solve/zsp_ctx.h``.

    Validated by probing for a real header: an unbuilt checkout and a wheel
    whose data files failed to stage both leave a plausible-looking directory
    behind, and handing that to ``gcc -I`` defers the failure to a confusing
    ``zsp_block_alloc.h: No such file or directory`` in generated code.

    Taken from the selected installation only. With nothing selected (no
    library anywhere) the first candidate holding headers answers, for
    compile-only consumers.

    NOTE ON THE COLLISION: dv-solve and zuspec-be-sw both ship a
    ``zsp_alloc.h`` and define ``struct zsp_alloc_s`` incompatibly. These
    directories are the include set for the SOLVER translation unit ONLY, and
    must never be merged into one ``-I`` list with the backend's. Namespacing
    the install under ``dv_solve/`` does not by itself protect against this,
    because the nested directory has to be on the include path for unqualified
    includes to work -- it is the per-TU segregation that keeps them apart.
    """
    inst = select_installation()
    if inst is not None:
        return _incdirs_in(inst)
    for inst in candidates():
        found = _incdirs_in(inst)
        if found is not None:
            return found
    return None


def find_svdirs() -> Optional[List[str]]:
    """The SystemVerilog package search path, or ``None``. Same selection
    rule as :func:`find_incdirs`."""
    inst = select_installation()
    if inst is not None:
        return _svdirs_in(inst)
    for inst in candidates():
        found = _svdirs_in(inst)
        if found is not None:
            return found
    return None


# ------------------------------------------------------------- diagnostics --


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


def _incomplete_error(inst: Installation, what: str, why: str) -> RuntimeError:
    """The selected installation lacks *what*. Never resolved by looking
    elsewhere -- that would pair artifacts from two solver builds."""
    fix = ("  - Complete that installation, or point ZSP_SOLVER_PATH at a "
           "complete one." if inst.kind == "override" else
           "  - Complete or remove that installation, or set ZSP_SOLVER_PATH "
           "to select a complete one.")
    return RuntimeError(
        "dv-solve: the selected installation (%s) has no %s.%s\n"
        "Other installations are not consulted: every artifact must come "
        "from the installation the solver library is loaded from.\n"
        "Fixes:\n%s" % (inst.describe(), what, (" " + why) if why else "", fix))


def require_library(stem: str, linkable: bool = False) -> str:
    """Like :func:`find_library` but raises an actionable ``RuntimeError``."""
    inst = select_installation()
    if inst is None:
        raise missing_artifact_error(
            "native library (%s)" % " / ".join(lib_patterns(stem)))
    hit = library_in(inst, stem, linkable)
    if hit is not None:
        return hit
    versioned = library_in(inst, stem) if linkable else None
    if versioned is not None:
        raise _incomplete_error(
            inst, "linkable %s" % lib_filename(stem),
            "It holds %s, which can be loaded but which -l%s cannot resolve. "
            "Add the linker name alongside it (e.g. ln -s %s %s)."
            % (versioned, stem, os.path.basename(versioned),
               lib_filename(stem)))
    raise _incomplete_error(inst, lib_filename(stem), "")


def require_incdirs() -> List[str]:
    found = find_incdirs()
    if found is not None:
        return found
    inst = select_installation()
    if inst is None:
        raise missing_artifact_error("C headers (%s)" % _SENTINEL_HEADER)
    raise _incomplete_error(inst, "C headers (%s)" % _SENTINEL_HEADER, "")


def require_svdirs() -> List[str]:
    found = find_svdirs()
    if found is not None:
        return found
    inst = select_installation()
    if inst is None:
        raise missing_artifact_error("SV sources (%s)" % _SENTINEL_SV)
    raise _incomplete_error(inst, "SV sources (%s)" % _SENTINEL_SV, "")
