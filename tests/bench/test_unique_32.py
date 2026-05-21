"""Benchmark: 32-variable all-different constraint.

Scales the uniqueness benchmark to 32 variables with 496 pairwise
inequality constraints.  Domain [0, 63] gives enough slack for a
solution but forces the solver to coordinate across all variables.

Variables (32):
  v0..v31 -- domain [0, 63]

Constraints:
  All pairs v_i != v_j for i != j  (496 inequalities)
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers

_N = 32
_DOM_HI = 63

# Build the class dynamically to avoid 496 hand-written constraints.
_fields = {f"v{i}": zdc.rand(domain=(0, _DOM_HI), default=i) for i in range(_N)}

# Build pairwise != constraints in groups to keep methods manageable.
_constraints = {}
for i in range(_N):
    def _make_constraint(idx):
        def _c(self):
            vi = getattr(self, f"v{idx}")
            for j in range(idx + 1, _N):
                vj = getattr(self, f"v{j}")
                assert vi != vj
        _c.__name__ = f"c_unique_{idx}"
        _c.__qualname__ = f"Unique32.c_unique_{idx}"
        return zdc.constraint(_c)
    _constraints[f"c_unique_{i}"] = _make_constraint(i)

Unique32 = zdc.dataclass(type("Unique32", (), {**_fields, **_constraints}))


def _check(sol):
    vals = [sol[f"v{i}"] for i in range(_N)]
    for v in vals:
        assert 0 <= v <= _DOM_HI, f"value {v} out of domain"
    assert len(set(vals)) == _N, f"duplicate values in {vals}"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_unique_32(solver, tmp_path):
    solver.bench(Unique32, validate=_check, tmp_path=tmp_path)
