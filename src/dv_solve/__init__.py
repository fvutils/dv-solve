import os
import platform

# The same file setup.py exec()s to supply the wheel's version, so an
# installed dv_solve and its distribution metadata cannot disagree. Importing
# it here is safe: the constraint that ruled out a dynamic attr: is a
# BUILD-time one (the package is not importable while the wheel is being
# built), and by the time this runs the package is installed.
from .__version__ import _pkg_version as __version__


def _pkg_dir():
    return os.path.dirname(os.path.abspath(__file__))


def _src_root():
    # packages/dv-solve/  (up from src/dv_solve/) in a source tree.
    return os.path.abspath(os.path.join(_pkg_dir(), "..", ".."))


def get_libs():
    """Names of the libraries a C consumer links against."""
    return ["dv_solve"]


def _lib_filename(stem):
    """The platform's file name for shared library *stem* (no directory)."""
    system = platform.system()
    if system == "Windows":
        return "%s.dll" % stem
    if system == "Darwin":
        return "lib%s.dylib" % stem
    return "lib%s.so" % stem


def _lib_search_dirs():
    """Every directory a dv-solve shared library may live in, best first.

    The two layouts disagree, in the same way ``get_incdirs()`` documents for
    headers:

      * installed wheel -- the libraries are staged at the package root, next to
        ``__init__.py`` (pyproject.toml's ``[[tool.ivpm-build.extra-data]]``
        entries use ``dst = ""``), which is where the ctypes loader looks;
      * source tree -- CMake writes them to ``build/<libdir>``, where ``<libdir>``
        is ``lib`` or ``lib64``; that is the very ``{libdir}`` the wheel stages
        *from*. ``build/`` itself holds the pre-install link output, so a
        checkout that has been built but not ``cmake --install``-ed resolves too.
    """
    src_root = _src_root()
    return [
        _pkg_dir(),                                         # installed wheel
        os.path.join(src_root, "build", "lib"),             # source tree
        os.path.join(src_root, "build", "lib64"),
        os.path.join(src_root, "build"),                    # built, not installed
    ]


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
    """
    name = _lib_filename("dv_solve")
    for d in _lib_search_dirs():
        if os.path.isfile(os.path.join(d, name)):
            return [d]
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
    """
    inc = os.path.join(_pkg_dir(), "share", "include")
    if os.path.isdir(inc):                                  # installed wheel
        nested = os.path.join(inc, "dv_solve")
        return [inc, nested] if os.path.isdir(nested) else [inc]
    return [os.path.join(_src_root(), "src", "c")]          # source tree


def get_svdirs():
    """SystemVerilog package search dir (zsp_dpi_pkg.sv, zsp_randomizer_pkg.sv)."""
    sv = os.path.join(_pkg_dir(), "share", "sv")
    if os.path.isdir(sv):                                   # installed wheel
        return [sv]
    return [os.path.join(_src_root(), "src", "sv")]         # source tree


def get_dpi_lib():
    """Absolute path to the DPI shared library for an SV simulator's -sv_lib.

    Searches the same places as :func:`get_libdirs` -- this library is staged
    into the wheel alongside ``libdv_solve`` and built into the same
    ``build/<libdir>`` in a checkout, so it had the same source-tree gap.
    """
    name = _lib_filename("dv_solve_dpi")
    for d in _lib_search_dirs():
        path = os.path.join(d, name)
        if os.path.isfile(path):
            return path
    return os.path.join(_pkg_dir(), name)
