import os
import platform

__version__ = '0.0.1'


def _pkg_dir():
    return os.path.dirname(os.path.abspath(__file__))


def _src_root():
    # packages/dv-solve/  (up from src/dv_solve/) in a source tree.
    return os.path.abspath(os.path.join(_pkg_dir(), "..", ".."))


def get_libs():
    """Names of the libraries a C consumer links against."""
    return ["dv_solve"]


def get_libdirs():
    """Directories containing the dv-solve shared libraries."""
    return [_pkg_dir()]


def get_deps():
    return []


def get_incdirs():
    """Directories containing the public dv-solve C headers."""
    inc = os.path.join(_pkg_dir(), "share", "include")
    if os.path.isdir(inc):                                  # installed wheel
        return [inc]
    return [os.path.join(_src_root(), "src", "c")]          # source tree


def get_svdirs():
    """SystemVerilog package search dir (zsp_dpi_pkg.sv, zsp_randomizer_pkg.sv)."""
    sv = os.path.join(_pkg_dir(), "share", "sv")
    if os.path.isdir(sv):                                   # installed wheel
        return [sv]
    return [os.path.join(_src_root(), "src", "sv")]         # source tree


def get_dpi_lib():
    """Absolute path to the DPI shared library for an SV simulator's -sv_lib."""
    system = platform.system()
    if system == "Windows":
        pref, ext = "", ".dll"
    elif system == "Darwin":
        pref, ext = "lib", ".dylib"
    else:
        pref, ext = "lib", ".so"
    return os.path.join(_pkg_dir(), "%sdv_solve_dpi%s" % (pref, ext))
