"""Benchmark: deep implication chain (8 levels).

A chain of 8 implications where each variable constrains the next.
BDD solvers must expand all 2^8 = 256 branch combinations; interval-
propagation solvers prune early.

Variables (8):
  a, b, c, d, e, f, g, h -- domain [0, 100]

Constraints (implication chain):
  a > 50  -> b < 30
  b < 30  -> c > 60
  c > 60  -> d < 20
  d < 20  -> e > 70
  e > 70  -> f < 25
  f < 25  -> g > 80
  g > 80  -> h < 15
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class ImplicationChain8:
    a: zdc.rand(domain=(0, 100), default=50)
    b: zdc.rand(domain=(0, 100), default=50)
    c: zdc.rand(domain=(0, 100), default=50)
    d: zdc.rand(domain=(0, 100), default=50)
    e: zdc.rand(domain=(0, 100), default=50)
    f: zdc.rand(domain=(0, 100), default=50)
    g: zdc.rand(domain=(0, 100), default=50)
    h: zdc.rand(domain=(0, 100), default=50)

    # a > 50 -> b < 30   (equivalently: a <= 50 OR b < 30)
    @zdc.constraint
    def c_ab(self):
        assert self.a <= 50 or self.b < 30

    # b < 30 -> c > 60
    @zdc.constraint
    def c_bc(self):
        assert self.b >= 30 or self.c > 60

    # c > 60 -> d < 20
    @zdc.constraint
    def c_cd(self):
        assert self.c <= 60 or self.d < 20

    # d < 20 -> e > 70
    @zdc.constraint
    def c_de(self):
        assert self.d >= 20 or self.e > 70

    # e > 70 -> f < 25
    @zdc.constraint
    def c_ef(self):
        assert self.e <= 70 or self.f < 25

    # f < 25 -> g > 80
    @zdc.constraint
    def c_fg(self):
        assert self.f >= 25 or self.g > 80

    # g > 80 -> h < 15
    @zdc.constraint
    def c_gh(self):
        assert self.g <= 80 or self.h < 15


def _check(sol):
    a, b, c, d = sol["a"], sol["b"], sol["c"], sol["d"]
    e, f, g, h = sol["e"], sol["f"], sol["g"], sol["h"]
    for name, v in sol.items():
        assert 0 <= v <= 100, f"{name}={v} out of range"

    if a > 50:
        assert b < 30, f"a={a}>50 but b={b}>=30"
    if b < 30:
        assert c > 60, f"b={b}<30 but c={c}<=60"
    if c > 60:
        assert d < 20, f"c={c}>60 but d={d}>=20"
    if d < 20:
        assert e > 70, f"d={d}<20 but e={e}<=70"
    if e > 70:
        assert f < 25, f"e={e}>70 but f={f}>=25"
    if f < 25:
        assert g > 80, f"f={f}<25 but g={g}<=80"
    if g > 80:
        assert h < 15, f"g={g}>80 but h={h}>=15"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_implication_chain_8(solver, tmp_path):
    solver.bench(ImplicationChain8, validate=_check, tmp_path=tmp_path)
