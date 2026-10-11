"""OpenTitan usb20 agent: a DATA packet's payload length.

usb20_item itself has no rand fields or constraints; of its subclasses
(token_pkt, data_pkt, sof_pkt, handshake_pkt) only data_pkt constrains
anything:

    class data_pkt extends usb20_item;
      rand byte unsigned data[]; // Dynamic array
      constraint data_c {
        data.size() <= 64;
      }

The solver variable is the array's size, an `int` (Verilator names it
__Vdata_size, and the case uses that name so equiv.py can pair them); a size
is never negative, which SV implies and Verilator states. The elements are
unconstrained and drawn after the size, outside the solver, so they are not
case variables.
"""
from dv_solve.problem import BIN_GTE, BIN_LTE

NAME = "usb20_item"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/dv/sv/usb20_agent/usb20_item.sv", "lines": "154-166"}
ABOUT = "a dynamic array's size bounded to [0:64] (signed int size)"

VARS = [("__Vdata_size", 32, True)]
DIST = {}
SOLVE_BEFORE = []


def build(b, v):
    size = b.expr_var(v["__Vdata_size"])
    int_ = lambda k: b.expr_const(k, is_signed=True, width=32)   # noqa: E731
    b.add_constraint(b.expr_binary(BIN_LTE, size, int_(64)))
    b.add_constraint(b.expr_binary(BIN_GTE, size, int_(0)))      # implied: size() >= 0
