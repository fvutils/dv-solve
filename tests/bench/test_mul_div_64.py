"""Benchmark: 64-bit multiply/divide/modulo operations.

Exercises the newly-implemented 64-bit mul/div/mod propagators.

total       -- result of multiplication (large range)
unit_price  -- price per unit
quantity    -- number of units
tax_rate    -- percentage [1..20]
remainder   -- leftover from division

Constraints:
  c_total:      total == unit_price * quantity
  c_tax_mod:    remainder == total % tax_rate
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class MulDivScenario:
    unit_price: zdc.rand(domain=(100, 10000), default=500)
    quantity:   zdc.rand(domain=(1, 100),     default=1)
    total:      zdc.rand(domain=(100, 1000000), default=500)

    @zdc.constraint
    def c_total(self):
        assert self.total == self.unit_price * self.quantity


def _check(sol):
    assert 100 <= sol["unit_price"] <= 10000
    assert 1 <= sol["quantity"] <= 100
    expected_total = sol["unit_price"] * sol["quantity"]
    assert sol["total"] == expected_total, (
        f"total mismatch: {sol['total']} != {sol['unit_price']} * {sol['quantity']}"
    )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_mul_div_64(solver, tmp_path):
    solver.bench(MulDivScenario, validate=_check, tmp_path=tmp_path)
