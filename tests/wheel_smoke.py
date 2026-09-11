"""Smoke test run by cibuildwheel against the installed wheel.

Confirms the bundled native library loads from the package directory (the
binary-wheel layout) without any source tree or environment overrides.
"""
import os
import sys

import dv_solve
from dv_solve.lib import _load_lib, _find_library


def main():
    lib_path = _find_library()
    if lib_path is None:
        print("FAIL: dv_solve native library not found in the installed wheel")
        return 1
    print("found native library:", lib_path)

    lib = _load_lib()
    if lib is None:
        print("FAIL: dv_solve native library found but failed to load")
        return 1
    print("loaded native library OK")

    # Discovery helpers should resolve to the installed wheel layout. Asserted,
    # not merely printed: these are what a C/SV consumer compiles and links
    # against, and they now probe several candidate directories (a source tree's
    # build/<libdir> among them), so "still the wheel layout here" is exactly the
    # property a wheel build has to confirm.
    libdirs = dv_solve.get_libdirs()
    print("libdirs:", libdirs)
    pkg_dir = os.path.dirname(os.path.abspath(dv_solve.__file__))
    if libdirs != [pkg_dir]:
        print("FAIL: get_libdirs() should be the package dir in a wheel, got", libdirs)
        return 1

    dpi = dv_solve.get_dpi_lib()
    print("dpi lib:", dpi)
    if not os.path.isfile(dpi):
        print("FAIL: get_dpi_lib() does not exist in the installed wheel:", dpi)
        return 1
    if os.path.dirname(dpi) != pkg_dir:
        print("FAIL: get_dpi_lib() should be in the package dir in a wheel:", dpi)
        return 1

    incdirs = dv_solve.get_incdirs()
    print("incdirs:", incdirs)
    if not any(os.path.isfile(os.path.join(d, "zsp_ctx.h")) for d in incdirs):
        print("FAIL: no reported include dir holds zsp_ctx.h:", incdirs)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
