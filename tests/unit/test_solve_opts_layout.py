"""The Python ``SolveOpts`` is the C ``SolveOpts``, field for field.

``solver_solve`` reads every field of the struct it is handed. A Python
mirror that is shorter than the C struct makes every solve read past the end
of the caller's buffer: until ``time_limit_ms`` was added here, the solve's
wall-clock limit was whatever four bytes followed the ctypes object.

The C layout is measured, not assumed: a probe is compiled against
``zsp_search.h`` and prints ``sizeof`` and each field's ``offsetof``.
"""
from __future__ import annotations

import ctypes
import os
import shutil
import subprocess

import pytest

from dv_solve.ctx import _SolveOpts

_SRC_C = os.path.join(os.path.dirname(__file__), "..", "..", "src", "c")

_FIELDS = ["seed", "max_conflicts", "max_restarts", "use_phase_save", "use_lcg",
           "fair_pick", "max_shave_iters", "time_limit_ms"]

_PROBE = r"""
#include <stdio.h>
#include <stddef.h>
#include "zsp_search.h"
int main(void) {
    printf("sizeof %%zu\n", sizeof(SolveOpts));
%s
    return 0;
}
"""


def _c_layout(tmp_path):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler")
    lines = "\n".join('    printf("%s %%zu\\n", offsetof(SolveOpts, %s));' % (f, f)
                      for f in _FIELDS)
    src = tmp_path / "probe.c"
    src.write_text(_PROBE % lines)
    exe = tmp_path / "probe"
    subprocess.run([cc, "-I", os.path.abspath(_SRC_C), str(src), "-o", str(exe)],
                   check=True, capture_output=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    return {k: int(v) for k, v in (ln.split() for ln in out.splitlines())}


def test_python_solve_opts_matches_c(tmp_path):
    c = _c_layout(tmp_path)
    assert ctypes.sizeof(_SolveOpts) == c["sizeof"]
    for f in _FIELDS:
        assert getattr(_SolveOpts, f).offset == c[f], f
