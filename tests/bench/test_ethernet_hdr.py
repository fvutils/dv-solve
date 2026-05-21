"""Benchmark: network packet header.

src_port  — source port;      0..65535
dst_port  — destination port; 0..65535
length    — payload length;   20..1500

Constraints (all natively compiled):
  c_ports_differ: src_port != dst_port   [var != var]
  c_src_fits:     length <= src_port     [var <= var]
  c_dst_fits:     dst_port <= length     [var <= var]
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class PacketHdr:
    src_port: zdc.rand(domain=(0, 65535), default=1024)
    dst_port: zdc.rand(domain=(0, 65535), default=80)
    length:   zdc.rand(domain=(20, 1500), default=64)

    @zdc.constraint
    def c_ports_differ(self):
        assert self.src_port != self.dst_port

    @zdc.constraint
    def c_src_fits(self):
        assert self.length <= self.src_port

    @zdc.constraint
    def c_dst_fits(self):
        assert self.dst_port <= self.length


def _check(sol):
    assert 0 <= sol["src_port"] <= 65535, f"src_port out of range: {sol['src_port']}"
    assert 0 <= sol["dst_port"] <= 65535, f"dst_port out of range: {sol['dst_port']}"
    assert 20 <= sol["length"] <= 1500, f"length out of range: {sol['length']}"
    assert sol["src_port"] != sol["dst_port"], (
        f"src_port == dst_port == {sol['src_port']}"
    )
    assert sol["length"] <= sol["src_port"], (
        f"src too small: src_port={sol['src_port']} length={sol['length']}"
    )
    assert sol["dst_port"] <= sol["length"], (
        f"dst too large: dst_port={sol['dst_port']} length={sol['length']}"
    )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_ethernet_hdr(solver, tmp_path):
    solver.bench(PacketHdr, validate=_check, tmp_path=tmp_path)
