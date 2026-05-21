"""Unit tests for the DPI shim chandle API.

Uses ctypes to call zsp_dpi_compile_b64 / zsp_dpi_solve_h /
zsp_dpi_get_value_h / zsp_dpi_release_h.
"""
from __future__ import annotations

import base64
import ctypes
import pytest

SOLVE_OK = 0
SOLVE_UNSAT = 1
BIN_LTE = 13
BIN_EQ = 10


def _setup_dpi(lib: ctypes.CDLL):
    """Wire argtypes for chandle DPI functions."""
    lib.zsp_dpi_compile_b64.restype = ctypes.c_void_p
    lib.zsp_dpi_compile_b64.argtypes = [ctypes.c_char_p]

    lib.zsp_dpi_solve_h.restype = ctypes.c_int
    lib.zsp_dpi_solve_h.argtypes = [ctypes.c_void_p, ctypes.c_longlong]

    lib.zsp_dpi_get_value_h.restype = ctypes.c_longlong
    lib.zsp_dpi_get_value_h.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.zsp_dpi_release_h.restype = None
    lib.zsp_dpi_release_h.argtypes = [ctypes.c_void_p]

    # Builder functions for constructing test problems
    lib.builder_create.restype = ctypes.c_void_p
    lib.builder_create.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
    lib.builder_destroy.restype = None
    lib.builder_destroy.argtypes = [ctypes.c_void_p]
    lib.builder_finalize.restype = ctypes.c_void_p
    lib.builder_finalize.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.builder_free_problem.restype = None
    lib.builder_free_problem.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t
    ]
    lib.builder_add_var.restype = ctypes.c_uint32
    lib.builder_add_var.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32,
        ctypes.c_uint8, ctypes.c_uint8,
        ctypes.c_int64, ctypes.c_int64,
    ]
    lib.builder_expr_var.restype = ctypes.c_uint32
    lib.builder_expr_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.builder_expr_const.restype = ctypes.c_uint32
    lib.builder_expr_const.argtypes = [
        ctypes.c_void_p, ctypes.c_int64, ctypes.c_uint8
    ]
    lib.builder_expr_binary.restype = ctypes.c_uint32
    lib.builder_expr_binary.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
    ]
    lib.builder_add_constraint.restype = ctypes.c_uint32
    lib.builder_add_constraint.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


def _build_2var_b64(lib):
    """Build a 2-var problem (a<=b, [0,100]) and return its base64 string."""
    b = lib.builder_create(0, None)
    assert b
    lib.builder_add_var(b, 0, 8, 0, 0, 100)
    lib.builder_add_var(b, 1, 8, 0, 0, 100)
    v0 = lib.builder_expr_var(b, 0)
    v1 = lib.builder_expr_var(b, 1)
    le = lib.builder_expr_binary(b, BIN_LTE, v0, v1)
    lib.builder_add_constraint(b, le)

    size = ctypes.c_size_t(0)
    sp_ptr = lib.builder_finalize(b, ctypes.byref(size))
    assert sp_ptr
    sz = size.value

    buf = (ctypes.c_uint8 * sz)()
    ctypes.memmove(buf, sp_ptr, sz)
    lib.builder_free_problem(b, sp_ptr, sz)
    lib.builder_destroy(b)

    raw = bytes(buf)
    return base64.b64encode(raw).decode("ascii")


# Load the DPI library
@pytest.fixture(scope="session")
def libdpi(tmp_path_factory):
    """Build and load libzsp_solver_dpi.so."""
    import shutil
    import subprocess
    from pathlib import Path

    pkg_dir = Path(__file__).parent.parent.parent

    if not shutil.which("cmake"):
        pytest.skip("cmake not found")

    build_dir = tmp_path_factory.mktemp("zsp_dpi_build")
    subprocess.run(
        ["cmake", str(pkg_dir), "-DCMAKE_BUILD_TYPE=Release"],
        cwd=build_dir, check=True, capture_output=True,
    )
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--parallel"],
        check=True, capture_output=True,
    )

    candidates = list(build_dir.glob("libzsp_solver_dpi.so*"))
    if not candidates:
        pytest.skip("libzsp_solver_dpi.so not built")
    candidates.sort(key=lambda p: len(p.name))
    return ctypes.CDLL(str(candidates[0]))


class TestDpiShim:
    @pytest.fixture(autouse=True)
    def setup(self, libdpi):
        self.lib = libdpi
        _setup_dpi(self.lib)

    def test_compile_solve_basic(self):
        """Compile 2 vars with a<=b, solve, verify."""
        b64 = _build_2var_b64(self.lib)

        ctx = self.lib.zsp_dpi_compile_b64(b64.encode("ascii"))
        assert ctx, "zsp_dpi_compile_b64 returned NULL"

        rc = self.lib.zsp_dpi_solve_h(ctx, 0x42)
        assert rc == 0, f"zsp_dpi_solve_h failed: {rc}"

        a = self.lib.zsp_dpi_get_value_h(ctx, 0)
        b = self.lib.zsp_dpi_get_value_h(ctx, 1)
        assert a <= b, f"a={a}, b={b}"
        assert 0 <= a <= 100
        assert 0 <= b <= 100

        self.lib.zsp_dpi_release_h(ctx)

    def test_compile_solve_reuse(self):
        """Compile once, solve 10 times with different seeds."""
        b64 = _build_2var_b64(self.lib)
        ctx = self.lib.zsp_dpi_compile_b64(b64.encode("ascii"))
        assert ctx

        for seed in range(10):
            rc = self.lib.zsp_dpi_solve_h(ctx, seed + 1)
            assert rc == 0
            a = self.lib.zsp_dpi_get_value_h(ctx, 0)
            b = self.lib.zsp_dpi_get_value_h(ctx, 1)
            assert a <= b

        self.lib.zsp_dpi_release_h(ctx)


    def test_null_handle(self):
        """Operations on NULL handle return error / 0."""
        rc = self.lib.zsp_dpi_solve_h(None, 0x42)
        assert rc == -1

        val = self.lib.zsp_dpi_get_value_h(None, 0)
        assert val == 0

    def test_bad_b64(self):
        """Invalid base64 returns NULL."""
        ctx = self.lib.zsp_dpi_compile_b64(b"!!!invalid!!!")
        assert ctx is None or ctx == 0

    def test_seed_determinism(self):
        """Same seed produces same solution."""
        b64 = _build_2var_b64(self.lib)

        results = []
        for _ in range(2):
            ctx = self.lib.zsp_dpi_compile_b64(b64.encode("ascii"))
            assert ctx
            rc = self.lib.zsp_dpi_solve_h(ctx, 0xABCD)
            assert rc == 0
            results.append((
                self.lib.zsp_dpi_get_value_h(ctx, 0),
                self.lib.zsp_dpi_get_value_h(ctx, 1),
            ))
            self.lib.zsp_dpi_release_h(ctx)

        assert results[0] == results[1], (
            f"Same seed produced different results: {results[0]} vs {results[1]}"
        )

    def test_get_value_out_of_range(self):
        """get_value_h with out-of-range var_id returns 0."""
        b64 = _build_2var_b64(self.lib)
        ctx = self.lib.zsp_dpi_compile_b64(b64.encode("ascii"))
        assert ctx
        rc = self.lib.zsp_dpi_solve_h(ctx, 1)
        assert rc == 0

        val = self.lib.zsp_dpi_get_value_h(ctx, 999)
        assert val == 0

        val = self.lib.zsp_dpi_get_value_h(ctx, -1)
        assert val == 0

        self.lib.zsp_dpi_release_h(ctx)
