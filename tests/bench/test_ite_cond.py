"""Benchmark: ITE / conditional constraint throughput.

Packet with a mode field and conditional data constraints.
Exercises the ITE value propagator and guard-gated infrastructure.

mode    -- packet mode (0 or 1)
data_a  -- data field used when mode=0
data_b  -- data field used when mode=1
result  -- selected data based on mode

Constraints:
  c_mode_selects: result == (mode == 0) ? data_a : data_b
    (approximated with: mode=0 -> result == data_a, mode=1 -> result == data_b)
  c_data_a_range: data_a in [100, 200]
  c_data_b_range: data_b in [1000, 2000]
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class ConditionalPacket:
    mode:   zdc.rand(domain=(0, 1),      default=0)
    data_a: zdc.rand(domain=(100, 200),  default=100)
    data_b: zdc.rand(domain=(1000, 2000), default=1000)
    result: zdc.rand(domain=(0, 2000),   default=100)

    @zdc.constraint
    def c_mode_a(self):
        if self.mode == 0:
            assert self.result == self.data_a

    @zdc.constraint
    def c_mode_b(self):
        if self.mode == 1:
            assert self.result == self.data_b


def _check(sol):
    assert sol["mode"] in (0, 1)
    assert 100 <= sol["data_a"] <= 200
    assert 1000 <= sol["data_b"] <= 2000
    if sol["mode"] == 0:
        assert sol["result"] == sol["data_a"], (
            f"mode=0 but result={sol['result']} != data_a={sol['data_a']}"
        )
    else:
        assert sol["result"] == sol["data_b"], (
            f"mode=1 but result={sol['result']} != data_b={sol['data_b']}"
        )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_ite_cond(solver, tmp_path):
    solver.bench(ConditionalPacket, validate=_check, tmp_path=tmp_path)
