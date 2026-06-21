"""Smoke test run by cibuildwheel against the installed wheel.

Confirms the bundled native library loads from the package directory (the
binary-wheel layout) without any source tree or environment overrides.
"""
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

    # Discovery helpers should resolve to the installed wheel layout.
    print("libdirs:", dv_solve.get_libdirs())
    print("dpi lib:", dv_solve.get_dpi_lib())
    return 0


if __name__ == "__main__":
    sys.exit(main())
