"""Phase 10 -- end-to-end tests for the native solver back-end.

Each test is parametrized over both "python" and "native" back-ends so
we can verify identical semantics.  The native back-end transparently falls
back to the Python solver when it encounters constraint types it cannot yet
compile natively (e.g. implications), so all tests should pass on both
back-ends.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest
from zuspec.dataclasses import (
    dataclass,
    rand,
    randc,
    constraint,
    randomize,
    randomize_with,
    RandomizationError,
    implies,
)

# ------------------------------------------------------------------ #
# Build / locate native library                                        #
# ------------------------------------------------------------------ #

_PKG_DIR = Path(__file__).parent.parent.parent  # packages/zuspec-solver


def _build_lib(build_dir: Path) -> Path:
    """Build libzsp_solver.so into build_dir via CMake."""
    build_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cmake", str(_PKG_DIR), "-DCMAKE_BUILD_TYPE=Release"],
        cwd=build_dir, check=True, capture_output=True,
    )
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--parallel"],
        check=True, capture_output=True,
    )
    hits = sorted(build_dir.glob("libzsp_solver.so*"), key=lambda p: len(p.name))
    if not hits:
        raise FileNotFoundError("libzsp_solver.so not found after build")
    return hits[0]


@pytest.fixture(scope="module")
def e2e_lib_path(tmp_path_factory):
    """Build libzsp_solver.so once for all e2e tests (module scope)."""
    if not shutil.which("cmake"):
        return None
    build_dir = tmp_path_factory.mktemp("zsp_e2e_build")
    try:
        return _build_lib(build_dir)
    except Exception:
        return None


def _make_native_available(lib_path: Path) -> bool:
    """Point _load_lib() at the freshly-built .so; reset the cache."""
    import dv_solve.lib as _lib_mod
    _lib_mod._LOAD_ATTEMPTED = False
    _lib_mod._LIB_CACHE = None
    os.environ["ZSP_SOLVER_PATH"] = str(lib_path.parent)
    return _lib_mod._load_lib() is not None


# ------------------------------------------------------------------ #
# Fixture: select back-end via ZSP_SOLVER_BACKEND env var             #
# ------------------------------------------------------------------ #

@pytest.fixture(params=["python", "native"])
def backend(request, monkeypatch, e2e_lib_path):
    """Parametrize tests over both solver back-ends."""
    name = request.param
    if name == "native":
        if e2e_lib_path is None or not _make_native_available(e2e_lib_path):
            pytest.skip("native solver library not available")
    monkeypatch.setenv("ZSP_SOLVER_BACKEND", name)
    yield name


# ------------------------------------------------------------------ #
# Data-class definitions (module-level so they can be reused)         #
# ------------------------------------------------------------------ #

@dataclass
class Unconstrained:
    a: rand(domain=(0, 255)) = 0
    b: rand(domain=(0, 255)) = 0


@dataclass
class Arithmetic:
    a: rand(domain=(0, 15)) = 0
    b: rand(domain=(0, 15)) = 0
    r: rand(domain=(0, 30)) = 0

    @constraint
    def sum_eq(self):
        assert self.r == self.a + self.b


@dataclass
class ComparisonChain:
    a: rand(domain=(0, 100)) = 0
    b: rand(domain=(0, 100)) = 0
    c: rand(domain=(0, 100)) = 0

    @constraint
    def ordered(self):
        assert self.a < self.b
        assert self.b < self.c


@dataclass
class ImplicationClass:
    """mode==1 implies addr is 4-byte aligned."""
    mode: rand(domain=(0, 1)) = 0
    addr: rand(domain=(0, 255)) = 0

    @constraint
    def word_align_when_write(self):
        assert implies(self.mode == 1, self.addr % 4 == 0)


@dataclass
class MultiConstraint:
    """Multiple constraints from separate @constraint methods: a < b, b < c, a >= 10."""
    a: rand(domain=(0, 100)) = 0
    b: rand(domain=(0, 100)) = 0
    c: rand(domain=(0, 100)) = 0

    @constraint
    def ab_ordered(self):
        assert self.a < self.b

    @constraint
    def bc_ordered(self):
        assert self.b < self.c

    @constraint
    def a_lower(self):
        assert self.a >= 10


@dataclass
class RandcClass:
    """randc field should cycle through its domain."""
    x: randc(domain=(0, 3)) = 0


@dataclass
class OrderedPair:
    lo: rand(domain=(0, 127)) = 0
    hi: rand(domain=(0, 127)) = 0

    @constraint
    def lo_lt_hi(self):
        assert self.lo < self.hi


@dataclass
class Contradictory:
    x: rand(domain=(0, 10)) = 0

    @constraint
    def impossible(self):
        assert self.x > 10


# ------------------------------------------------------------------ #
# Tests                                                               #
# ------------------------------------------------------------------ #

class TestE2E:
    """End-to-end parity tests across both solver back-ends."""

    def test_unconstrained(self, backend):
        """Unconstrained fields are assigned values within their domains."""
        obj = Unconstrained()
        randomize(obj)
        assert 0 <= obj.a <= 255
        assert 0 <= obj.b <= 255

    def test_arithmetic_constraint(self, backend):
        """r == a + b is enforced correctly."""
        obj = Arithmetic()
        for _ in range(10):
            randomize(obj, seed=None)
            assert obj.r == obj.a + obj.b, (
                f"a={obj.a}, b={obj.b}, r={obj.r}"
            )

    def test_comparison_chain(self, backend):
        """a < b < c is enforced across multiple calls."""
        obj = ComparisonChain()
        for _ in range(10):
            randomize(obj, seed=None)
            assert obj.a < obj.b < obj.c, (
                f"a={obj.a}, b={obj.b}, c={obj.c}"
            )

    def test_implication_constraint(self, backend):
        """implies(mode==1, addr%4==0) holds for every result.

        The native back-end falls back to Python transparently here.
        """
        obj = ImplicationClass()
        for _ in range(20):
            randomize(obj, seed=None)
            if obj.mode == 1:
                assert obj.addr % 4 == 0, (
                    f"mode=1 but addr={obj.addr} is not word-aligned"
                )

    def test_multi_constraint(self, backend):
        """Constraints from multiple @constraint methods are all honoured."""
        obj = MultiConstraint()
        for _ in range(10):
            randomize(obj, seed=None)
            assert obj.a >= 10, f"a={obj.a}"
            assert obj.a < obj.b, f"a={obj.a}, b={obj.b}"
            assert obj.b < obj.c, f"b={obj.b}, c={obj.c}"

    def test_randc_field_in_domain(self, backend):
        """randc field always returns a value within its declared domain."""
        obj = RandcClass()
        for _ in range(10):
            randomize(obj, seed=None)
            assert 0 <= obj.x <= 3, f"x={obj.x} out of domain [0,3]"

    def test_ordered_pair(self, backend):
        """lo < hi simple ordered-pair constraint."""
        obj = OrderedPair()
        for _ in range(10):
            randomize(obj, seed=None)
            assert obj.lo < obj.hi, f"lo={obj.lo}, hi={obj.hi}"

    def test_unsat_raises(self, backend):
        """Contradictory constraints raise RandomizationError."""
        obj = Contradictory()
        with pytest.raises(RandomizationError):
            randomize(obj)

    def test_seeded_reproducibility(self, backend):
        """Same seed produces the same result on every call."""
        obj1 = Arithmetic()
        obj2 = Arithmetic()
        randomize(obj1, seed=12345)
        randomize(obj2, seed=12345)
        assert obj1.a == obj2.a
        assert obj1.b == obj2.b
        assert obj1.r == obj2.r

    def test_randomize_with_inline_constraint(self, backend):
        """randomize_with inline constraints are honoured."""
        obj = Unconstrained()
        with randomize_with(obj, seed=99):
            assert obj.a < 10
            assert obj.b > 200
        assert obj.a < 10, f"a={obj.a} violates inline constraint a < 10"
        assert obj.b > 200, f"b={obj.b} violates inline constraint b > 200"
