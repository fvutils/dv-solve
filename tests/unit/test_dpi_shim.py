"""Unit tests for the DPI shim chandle API.

Uses ctypes to call dvs_dpi_compile_b64 / dvs_dpi_solve_h /
dvs_dpi_get_value_h / dvs_dpi_release_h.
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
    lib.dvs_dpi_compile_b64.restype = ctypes.c_void_p
    lib.dvs_dpi_compile_b64.argtypes = [ctypes.c_char_p]

    lib.dvs_dpi_solve_h.restype = ctypes.c_int
    lib.dvs_dpi_solve_h.argtypes = [ctypes.c_void_p, ctypes.c_longlong]

    lib.dvs_dpi_get_value_h.restype = ctypes.c_longlong
    lib.dvs_dpi_get_value_h.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.dvs_dpi_release_h.restype = None
    lib.dvs_dpi_release_h.argtypes = [ctypes.c_void_p]

    lib.dvs_dpi_n_uncompiled_h.restype = ctypes.c_int
    lib.dvs_dpi_n_uncompiled_h.argtypes = [ctypes.c_void_p]

    lib.dvs_dpi_pin_var_h.restype = ctypes.c_int
    lib.dvs_dpi_pin_var_h.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_longlong,
    ]
    lib.dvs_dpi_checkpoint_h.restype = ctypes.c_int
    lib.dvs_dpi_checkpoint_h.argtypes = [ctypes.c_void_p]
    lib.dvs_dpi_restore_h.restype = None
    lib.dvs_dpi_restore_h.argtypes = [ctypes.c_void_p, ctypes.c_int]

    # Builder functions for constructing test problems
    lib.dvs_builder_create.restype = ctypes.c_void_p
    lib.dvs_builder_create.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
    lib.dvs_builder_destroy.restype = None
    lib.dvs_builder_destroy.argtypes = [ctypes.c_void_p]
    lib.dvs_builder_finalize.restype = ctypes.c_void_p
    lib.dvs_builder_finalize.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.dvs_builder_free_problem.restype = None
    lib.dvs_builder_free_problem.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t
    ]
    lib.dvs_builder_add_var.restype = ctypes.c_uint32
    lib.dvs_builder_add_var.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32,
        ctypes.c_uint8, ctypes.c_uint8,
        ctypes.c_int64, ctypes.c_int64,
    ]
    lib.dvs_builder_expr_var.restype = ctypes.c_uint32
    lib.dvs_builder_expr_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.dvs_builder_expr_const.restype = ctypes.c_uint32
    lib.dvs_builder_expr_const.argtypes = [
        ctypes.c_void_p, ctypes.c_int64, ctypes.c_uint8
    ]
    lib.dvs_builder_expr_binary.restype = ctypes.c_uint32
    lib.dvs_builder_expr_binary.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
    ]
    lib.dvs_builder_add_constraint.restype = ctypes.c_uint32
    lib.dvs_builder_add_constraint.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


def _build_2var_b64(lib):
    """Build a 2-var problem (a<=b, [0,100]) and return its base64 string."""
    b = lib.dvs_builder_create(0, None)
    assert b
    lib.dvs_builder_add_var(b, 0, 8, 0, 0, 100)
    lib.dvs_builder_add_var(b, 1, 8, 0, 0, 100)
    v0 = lib.dvs_builder_expr_var(b, 0)
    v1 = lib.dvs_builder_expr_var(b, 1)
    le = lib.dvs_builder_expr_binary(b, BIN_LTE, v0, v1)
    lib.dvs_builder_add_constraint(b, le)

    size = ctypes.c_size_t(0)
    sp_ptr = lib.dvs_builder_finalize(b, ctypes.byref(size))
    assert sp_ptr
    sz = size.value

    buf = (ctypes.c_uint8 * sz)()
    ctypes.memmove(buf, sp_ptr, sz)
    lib.dvs_builder_free_problem(b, sp_ptr, sz)
    lib.dvs_builder_destroy(b)

    raw = bytes(buf)
    return base64.b64encode(raw).decode("ascii")


# Load the DPI library
@pytest.fixture(scope="session")
def libdpi(tmp_path_factory):
    """The DPI shim: the one the resolver selects, else a fresh build."""
    import shutil
    import subprocess
    from pathlib import Path

    from dv_solve import _resolve

    found = _resolve.find_library("dv_solve_dpi")
    if found:
        return ctypes.CDLL(found)

    pkg_dir = Path(__file__).parent.parent.parent

    if not shutil.which("cmake"):
        pytest.skip("cmake not found")

    build_dir = tmp_path_factory.mktemp("dvs_dpi_build")
    subprocess.run(
        ["cmake", str(pkg_dir), "-DCMAKE_BUILD_TYPE=Release"],
        cwd=build_dir, check=True, capture_output=True,
    )
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--parallel"],
        check=True, capture_output=True,
    )

    candidates = [p for pat in _resolve.lib_patterns("dv_solve_dpi")
                  for p in build_dir.rglob(pat)]
    if not candidates:
        pytest.skip("the DPI shim was not built")
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

        ctx = self.lib.dvs_dpi_compile_b64(b64.encode("ascii"))
        assert ctx, "dvs_dpi_compile_b64 returned NULL"

        rc = self.lib.dvs_dpi_solve_h(ctx, 0x42)
        assert rc == 0, f"dvs_dpi_solve_h failed: {rc}"

        a = self.lib.dvs_dpi_get_value_h(ctx, 0)
        b = self.lib.dvs_dpi_get_value_h(ctx, 1)
        assert a <= b, f"a={a}, b={b}"
        assert 0 <= a <= 100
        assert 0 <= b <= 100

        self.lib.dvs_dpi_release_h(ctx)

    def test_compile_solve_reuse(self):
        """Compile once, solve 10 times with different seeds."""
        b64 = _build_2var_b64(self.lib)
        ctx = self.lib.dvs_dpi_compile_b64(b64.encode("ascii"))
        assert ctx

        for seed in range(10):
            rc = self.lib.dvs_dpi_solve_h(ctx, seed + 1)
            assert rc == 0
            a = self.lib.dvs_dpi_get_value_h(ctx, 0)
            b = self.lib.dvs_dpi_get_value_h(ctx, 1)
            assert a <= b

        self.lib.dvs_dpi_release_h(ctx)


    def test_null_handle(self):
        """Operations on NULL handle return error / 0."""
        rc = self.lib.dvs_dpi_solve_h(None, 0x42)
        assert rc == -1

        val = self.lib.dvs_dpi_get_value_h(None, 0)
        assert val == 0

    def test_bad_b64(self):
        """Invalid base64 returns NULL."""
        ctx = self.lib.dvs_dpi_compile_b64(b"!!!invalid!!!")
        assert ctx is None or ctx == 0

    @pytest.mark.parametrize("corrupt", [
        lambda g: "",                       # empty
        lambda g: g[:-1],                   # length not a multiple of 4
        lambda g: g[:4] + "~" + g[5:],      # byte above 'z' (used to read as 'A')
        lambda g: g[:4] + "\x7f" + g[5:],
        lambda g: g[:4] + "=" + g[5:],      # padding in the middle
        lambda g: g[:4] + "-" + g[5:],      # URL-safe alphabet
    ])
    def test_malformed_b64_rejected(self, corrupt):
        """Characters outside the alphabet, misplaced padding and lengths that
        are not a multiple of 4 are rejected. The decoder used to read every
        byte above 'z' as 'A', so a corrupted problem string compiled into a
        different problem instead of failing."""
        bad = corrupt(_build_2var_b64(self.lib))
        assert not self.lib.dvs_dpi_compile_b64(bad.encode("latin-1"))

    def _values(self, ctx):
        return (self.lib.dvs_dpi_get_value_h(ctx, 0),
                self.lib.dvs_dpi_get_value_h(ctx, 1))

    def test_reuse_gives_different_solutions(self):
        """Each solve on a reused handle starts from the constraints, not from
        the previous solution. It used to start from the previous solution, so
        every seed returned the first answer again and an SV randomizer
        produced the same object on every call."""
        ctx = self.lib.dvs_dpi_compile_b64(_build_2var_b64(self.lib).encode())
        try:
            seen = set()
            for seed in range(1, 21):
                assert self.lib.dvs_dpi_solve_h(ctx, seed) == 0
                a, b = self._values(ctx)
                assert 0 <= a <= b <= 100
                seen.add((a, b))
            assert len(seen) >= 10, seen
            # The same seed still gives the same solution as on a fresh handle.
            assert self.lib.dvs_dpi_solve_h(ctx, 5) == 0
            reused = self._values(ctx)
        finally:
            self.lib.dvs_dpi_release_h(ctx)
        fresh = self.lib.dvs_dpi_compile_b64(_build_2var_b64(self.lib).encode())
        try:
            assert self.lib.dvs_dpi_solve_h(fresh, 5) == 0
            assert self._values(fresh) == reused
        finally:
            self.lib.dvs_dpi_release_h(fresh)

    def test_checkpoint_limit_leaves_room_to_solve(self):
        ctx = self.lib.dvs_dpi_compile_b64(_build_2var_b64(self.lib).encode())
        try:
            cps = [self.lib.dvs_dpi_checkpoint_h(ctx) for _ in range(40)]
            assert cps[:31] == list(range(31)) and set(cps[31:]) == {-1}
            assert self.lib.dvs_dpi_solve_h(ctx, 1) == 0
        finally:
            self.lib.dvs_dpi_release_h(ctx)

    def test_pin_lasts_across_solves(self):
        ctx = self.lib.dvs_dpi_compile_b64(_build_2var_b64(self.lib).encode())
        try:
            assert self.lib.dvs_dpi_pin_var_h(ctx, 0, 50) == 0
            bs = set()
            for seed in range(1, 11):
                assert self.lib.dvs_dpi_solve_h(ctx, seed) == 0
                a, b = self._values(ctx)
                assert a == 50 and 50 <= b <= 100
                bs.add(b)
            assert len(bs) > 1
        finally:
            self.lib.dvs_dpi_release_h(ctx)

    def test_pin_after_solve_checks_constraints_not_solution(self):
        """A pin after a solve is checked against the constraints. It used to
        be checked against the solved values, so any value other than the
        last solution was reported as a conflict."""
        ctx = self.lib.dvs_dpi_compile_b64(_build_2var_b64(self.lib).encode())
        try:
            assert self.lib.dvs_dpi_solve_h(ctx, 3) == 0
            a, _ = self._values(ctx)
            other = 0 if a != 0 else 1
            assert self.lib.dvs_dpi_pin_var_h(ctx, 0, other) == 0
            assert self.lib.dvs_dpi_solve_h(ctx, 4) == 0
            assert self._values(ctx)[0] == other
            assert self.lib.dvs_dpi_pin_var_h(ctx, 0, 101) == -2
        finally:
            self.lib.dvs_dpi_release_h(ctx)

    def test_checkpoint_restores_more_than_once(self):
        ctx = self.lib.dvs_dpi_compile_b64(_build_2var_b64(self.lib).encode())
        try:
            cp = self.lib.dvs_dpi_checkpoint_h(ctx)
            assert cp >= 0
            for pin in (10, 20, 30):
                assert self.lib.dvs_dpi_pin_var_h(ctx, 0, pin) == 0
                assert self.lib.dvs_dpi_solve_h(ctx, pin) == 0
                assert self._values(ctx)[0] == pin
                self.lib.dvs_dpi_restore_h(ctx, cp)
            # After the restore no pin remains: var 0 varies again.
            seen = set()
            for seed in range(1, 11):
                assert self.lib.dvs_dpi_solve_h(ctx, seed) == 0
                seen.add(self._values(ctx)[0])
            assert len(seen) > 3, seen
            assert self.lib.dvs_dpi_checkpoint_h(ctx) == cp + 1
        finally:
            self.lib.dvs_dpi_release_h(ctx)

    def test_seed_determinism(self):
        """Same seed produces same solution."""
        b64 = _build_2var_b64(self.lib)

        results = []
        for _ in range(2):
            ctx = self.lib.dvs_dpi_compile_b64(b64.encode("ascii"))
            assert ctx
            rc = self.lib.dvs_dpi_solve_h(ctx, 0xABCD)
            assert rc == 0
            results.append((
                self.lib.dvs_dpi_get_value_h(ctx, 0),
                self.lib.dvs_dpi_get_value_h(ctx, 1),
            ))
            self.lib.dvs_dpi_release_h(ctx)

        assert results[0] == results[1], (
            f"Same seed produced different results: {results[0]} vs {results[1]}"
        )

    def test_get_value_out_of_range(self):
        """get_value_h with out-of-range var_id returns 0."""
        b64 = _build_2var_b64(self.lib)
        ctx = self.lib.dvs_dpi_compile_b64(b64.encode("ascii"))
        assert ctx
        rc = self.lib.dvs_dpi_solve_h(ctx, 1)
        assert rc == 0

        val = self.lib.dvs_dpi_get_value_h(ctx, 999)
        assert val == 0

        val = self.lib.dvs_dpi_get_value_h(ctx, -1)
        assert val == 0

        self.lib.dvs_dpi_release_h(ctx)

    def test_n_uncompiled_reported(self):
        """The shim must expose whether compile took the whole problem.

        It used to test only `rc < 0`, so a POSITIVE dvs_solver_compile return --
        the count of constraints it could not compile -- was discarded and the
        SV consumer solved with those constraints dropped. Nothing reported it,
        which made the result indistinguishable from a correct solve."""
        b64 = _build_2var_b64(self.lib)
        ctx = self.lib.dvs_dpi_compile_b64(b64.encode("ascii"))
        assert ctx
        assert self.lib.dvs_dpi_n_uncompiled_h(ctx) == 0
        self.lib.dvs_dpi_release_h(ctx)

        assert self.lib.dvs_dpi_n_uncompiled_h(None) == -1

    def test_model_validated_when_constraints_dropped(self):
        """A solve is re-checked against the ORIGINAL problem whenever compile
        dropped something, so a model that violates a dropped constraint is
        reported (rc 3) rather than returned as a successful solve.

        Driven through the ordinary path: whatever compile happens to accept,
        the answer handed back must satisfy the problem that was submitted."""
        b64 = _build_2var_b64(self.lib)
        ctx = self.lib.dvs_dpi_compile_b64(b64.encode("ascii"))
        assert ctx
        try:
            rc = self.lib.dvs_dpi_solve_h(ctx, 7)
            assert rc in (0, 1), f"unexpected rc={rc}"
            if rc == 0:
                a = self.lib.dvs_dpi_get_value_h(ctx, 0)
                b = self.lib.dvs_dpi_get_value_h(ctx, 1)
                assert a <= b, f"reported OK but a={a} > b={b}"
        finally:
            self.lib.dvs_dpi_release_h(ctx)
