import os
import platform

from . import _resolve

# The same file setup.py exec()s to supply the wheel's version, so an
# installed dv_solve and its distribution metadata cannot disagree. Importing
# it here is safe: the constraint that ruled out a dynamic attr: is a
# BUILD-time one (the package is not importable while the wheel is being
# built), and by the time this runs the package is installed.
from .__version__ import _pkg_version as __version__


def _pkg_dir():
    # Delegated so that ``_resolve`` is the ONE place either location is
    # decided. Keeping a second copy here meant the fallback paths below could
    # name a different installation than the search itself had considered.
    return _resolve._pkg_dir()


def _src_root():
    # packages/dv-solve/  (up from src/dv_solve/) in a source tree.
    return _resolve._src_root()


def get_libs():
    """Names of the libraries a C consumer links against."""
    return ["dv_solve"]


def _lib_filename(stem):
    """The platform's file name for shared library *stem* (no directory)."""
    return _resolve.lib_filename(stem)


def _lib_search_dirs():
    """Every directory a dv-solve shared library may live in, best first.

    Now just the shared contract in :mod:`dv_solve._resolve`. This list used to
    be maintained here independently of the ctypes loader's, and the two had
    drifted: the loader also honoured ``ZSP_SOLVER_PATH``, the package's
    ``lib/`` subdirectory and three alternate build-directory names, none of
    which this function knew about. On a host using any of them the solver ran
    out of one installation and generated C linked against another.
    """
    return _resolve.lib_search_dirs()


def get_libdirs():
    """Directories containing the dv-solve shared libraries.

    Returning only the package directory described a layout a *source tree* does
    not have, so a consumer that compiled against a checkout failed to link:

        /usr/bin/ld: cannot find -ldv_solve

    which is how pssc's generated ``libpssc_scenario.so`` failed once dv-solve
    was installed editable rather than from a wheel. The same reasoning, and the
    same fix, as ``get_incdirs()`` -- see its docstring.

    Probes for the library rather than the directory: in a source tree
    ``build/lib`` exists as soon as CMake configures, well before anything is
    linked into it.

    Only the UNVERSIONED library name counts here, because that is the one
    ``-ldv_solve`` resolves. A directory holding nothing but
    ``libdv_solve.so.1`` satisfies the ctypes loader and would have satisfied
    the old probe, but fails at link time -- so the loader and the linker have
    to apply different acceptance rules to stay on the same installation.
    """
    found = _resolve.find_library("dv_solve", linkable=True)
    if found is not None:
        return [os.path.dirname(found)]
    # Nothing built anywhere. The package directory is the installed-wheel
    # answer and keeps the failure identical to what a missing wheel library
    # already produces, rather than naming a build tree that does not exist.
    return [_pkg_dir()]


def get_deps():
    return []


def get_incdirs():
    """Directories containing the public dv-solve C headers.

    The wheel case returns BOTH ``share/include`` and ``share/include/dv_solve``
    because the two trees disagree on layout and only the nested one actually
    holds headers:

      * source tree -- headers sit FLAT in ``src/c``, so ``#include
        "zsp_ctx.h"`` resolves;
      * installed wheel -- CMake puts them in ``include/dv_solve``
        (CMakeLists.txt: ``DESTINATION include/dv_solve``), so that same
        unqualified include does NOT resolve against the base directory, and
        the base directory on its own contains no headers at all.

    Returning only the base therefore described a layout the wheel does not
    have. A consumer following this API could compile against a checkout and
    then fail against the released wheel with

        fatal error: zsp_block_alloc.h: No such file or directory

    which is exactly how pssc's generated ``pssc_solve.c`` fails, since it emits
    unqualified includes.

    NOTE ON THE COLLISION: dv-solve and zuspec-be-sw both ship a ``zsp_alloc.h``
    and they define ``struct zsp_alloc_s`` incompatibly. That is why these
    headers are namespaced under ``dv_solve/`` in the install tree, and why a
    consumer must compile the SOLVER translation unit with these directories and
    the runtime/component translation units with be-sw's -- not merge both into
    one ``-I`` set. Adding the nested directory here does not weaken that: it is
    the per-TU segregation, not the absence of this path, that keeps the two
    ``zsp_alloc.h`` files apart.

    Probes for an actual header rather than testing ``isdir``. A staging bug
    that creates ``share/include`` but populates nothing under it used to pass
    the directory test and then fail deep inside a consumer's compile.
    """
    found = _resolve.find_incdirs()
    if found is not None:
        return found
    # Nothing found: name the installed-wheel location, so the failure is the
    # same "no headers here" a missing wheel already produces.
    return [os.path.join(_pkg_dir(), "share", "include")]


def get_svdirs():
    """SystemVerilog package search dir (zsp_dpi_pkg.sv, zsp_randomizer_pkg.sv)."""
    found = _resolve.find_svdirs()
    if found is not None:
        return found
    return [os.path.join(_pkg_dir(), "share", "sv")]


def get_dpi_lib():
    """Absolute path to the DPI shared library for an SV simulator's -sv_lib.

    Searches the same places as :func:`get_libdirs` -- this library is staged
    into the wheel alongside ``libdv_solve`` and built into the same
    ``build/<libdir>`` in a checkout, so it had the same source-tree gap.

    Resolved by its OWN filename rather than by appending to whatever directory
    the core library was found in: the two are staged together today, but a
    ``-sv_lib`` pointed at a path that merely ought to exist fails inside the
    simulator's elaborator, a long way from anything that names dv-solve.
    """
    found = _resolve.find_library("dv_solve_dpi")
    if found is not None:
        return found
    return os.path.join(_pkg_dir(), _lib_filename("dv_solve_dpi"))


def resolve_report():
    """What this installation actually resolved to, for diagnostics.

    Deployment problems in this area are all of the form "which installation
    did it pick?", and answering that from the outside means replicating the
    search. Returns a dict of plain strings/lists, safe to print or assert on.
    """
    return {
        "package_dir": _pkg_dir(),
        "override": os.environ.get("ZSP_SOLVER_PATH"),
        "search_dirs": list(_lib_search_dirs()),
        "core_lib": _resolve.find_library("dv_solve"),
        "link_dirs": get_libdirs(),
        "dpi_lib": _resolve.find_library("dv_solve_dpi"),
        "incdirs": _resolve.find_incdirs(),
        "svdirs": _resolve.find_svdirs(),
    }
