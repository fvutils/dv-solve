"""OpenTitan jtag_riscv_agent: a CSR access sequence's op, address and data.

    rand bit [  DMI_OPW-1:0] op;                  // 2 bits
    rand bit [DMI_DATAW-1:0] data;                // 32 bits
    // This is not DTM address, but address for the CSR registers
    rand bit [DMI_DATAW+1:0] addr;                // 34 bits
    rand bit                 do_write;
    constraint op_c {
      do_write == 1 -> op == DmiWrite;
      do_write == 0 -> op == DmiRead;
    }

jtag_op_e: DmiStatus = 'h0, DmiRead = 'h1, DmiWrite = 'h2 (enum logic
[DMI_OPW-1:0]). op_c is the class's only block; addr and data are free here
(the body narrows them in a randomize-with of the jtag_riscv_item, another
class). Verilator leaves the free vars out of the query; the case keeps them.
"""
from dv_solve.problem import BIN_EQ

from tests.perf.svcorpus.case import implies

NAME = "jtag_riscv_csr"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/dv/sv/jtag_riscv_agent/seq_lib/jtag_riscv_csr_seq.sv", "lines": "6-17"}
ABOUT = "op follows a do_write bit; 34-bit address and 32-bit data free"

VARS = [("op", 2, False), ("data", 32, False), ("addr", 34, False), ("do_write", 1, False)]
DIST = {}
SOLVE_BEFORE = []

DMI_READ, DMI_WRITE = 0x1, 0x2


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    i32 = lambda x: b.expr_const(x, is_signed=True, width=32)   # noqa: E731
    eq = lambda a, c: b.expr_binary(BIN_EQ, a, c)          # noqa: E731
    b.add_constraint(implies(b, eq(var("do_write"), i32(1)),
                             eq(var("op"), b.expr_const(DMI_WRITE, width=2))))
    b.add_constraint(implies(b, eq(var("do_write"), i32(0)),
                             eq(var("op"), b.expr_const(DMI_READ, width=2))))
