"""Smoke test run by cibuildwheel against the installed wheel.

Confirms the bundled native library loads from the package directory (the
binary-wheel layout) without any source tree or environment overrides, and
that dv-solve-smt2 was installed into the environment's scripts directory and
runs from there.
"""
import os
import platform
import shutil
import subprocess
import sys
import sysconfig
import tempfile

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
    if not any(os.path.isfile(os.path.join(d, "dv_solve.h")) for d in incdirs):
        print("FAIL: no reported include dir holds dv_solve.h:", incdirs)
        return 1

    rc = check_cxx_runtime_bundled()
    if rc:
        return rc
    return check_smt2()


def check_cxx_runtime_bundled():
    """On Windows the wheel must carry MSVCP140.dll (delvewheel, see
    pyproject.toml). Loading the library is not evidence of that: the CI
    runners have the Visual C++ Redistributable installed system-wide, so the
    load succeeds with or without the bundled copy."""
    if platform.system() != "Windows":
        return 0
    from importlib import metadata
    names = [f.name.lower() for f in (metadata.distribution("dv-solve").files or [])]
    bundled = [n for n in names if n.startswith("msvcp140") and n.endswith(".dll")]
    print("bundled C++ runtime:", bundled)
    if not bundled:
        print("FAIL: the Windows wheel does not bundle MSVCP140.dll")
        return 1
    return 0


SMT2_PROBLEM = """\
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(assert (bvugt x #x10))
(check-sat)
"""


def check_smt2():
    """dv-solve-smt2 is in the environment's scripts dir, on PATH, and works.

    Not on Windows yet: the tool needs POSIX headers, so the Windows wheel
    ships without it and get_smt2_exe() must say so. Asserting that, rather
    than skipping, means a port that starts shipping it fails here and gets
    this check turned on.
    """
    if platform.system() == "Windows":
        try:
            exe = dv_solve.get_smt2_exe()
        except RuntimeError as e:
            print("dv-solve-smt2 not in the Windows wheel, as expected:",
                  str(e).splitlines()[0])
            return 0
        print("FAIL: the Windows wheel now has dv-solve-smt2 (%s); enable the "
              "check in tests/wheel_smoke.py" % exe)
        return 1

    scripts = sysconfig.get_path("scripts")
    expected = os.path.join(scripts, "dv-solve-smt2")
    if not os.path.isfile(expected):
        print("FAIL: dv-solve-smt2 not installed in the scripts dir:", expected)
        return 1
    if not os.access(expected, os.X_OK):
        print("FAIL: installed dv-solve-smt2 is not executable:", expected)
        return 1
    print("dv-solve-smt2:", expected)

    exe = dv_solve.get_smt2_exe()
    if os.path.realpath(exe) != os.path.realpath(expected):
        print("FAIL: get_smt2_exe() should be the wheel's copy, got", exe)
        return 1

    # On PATH whenever the environment's scripts dir is (an activated venv).
    on_path = shutil.which("dv-solve-smt2")
    print("on PATH:", on_path)
    if scripts in os.environ.get("PATH", "").split(os.pathsep):
        if on_path is None or (os.path.realpath(on_path)
                               != os.path.realpath(expected)):
            print("FAIL: PATH holds %s but which() found %s" % (scripts, on_path))
            return 1

    r = subprocess.run([exe, "--version"], capture_output=True, text=True)
    print("--version:", r.stdout.strip())
    # BASE, not __version__: CI appends a SUFFIX to the package version of a
    # non-release build, while the tool reports BASE (CMakeLists.txt).
    from dv_solve.__version__ import BASE
    if r.returncode != 0 or r.stdout.split() != ["dv-solve-smt2", BASE]:
        print("FAIL: --version exit=%d stdout=%r stderr=%r"
              % (r.returncode, r.stdout, r.stderr))
        return 1

    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "probe.smt2")
        with open(path, "w") as fh:
            fh.write(SMT2_PROBLEM)
        r = subprocess.run([exe, path], capture_output=True, text=True)
    print("probe:", r.stdout.strip())
    if r.returncode != 0 or r.stdout.split()[:1] != ["sat"]:
        print("FAIL: probe exit=%d stdout=%r stderr=%r"
              % (r.returncode, r.stdout, r.stderr))
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
