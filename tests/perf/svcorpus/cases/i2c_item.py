"""OpenTitan i2c agent: an item's FMTFIFO control flags.

    int wait_cycles = 8;
    rand bit [7:0] fbyte;
    rand bit       nakok, rcont, read, stop, start;
    constraint fbyte_c { fbyte inside {[0 : 127] }; }
    constraint rcont_c {
       solve read, stop before rcont;
       // for read request, rcont and stop must be complementary set
       if (read) {
         rcont == ~stop;
       } else {
         rcont dist { 1 :/ 1, 0 :/ 2 };
       }
    }
    constraint wait_cycles_c { wait_cycles == 8; }

wait_cycles is not rand: wait_cycles_c only checks its default (8), a
constant-true constraint here. The `dist` sits in the else branch: its
membership is part of that branch; DIST records its weights unconditionally
(the format has no condition). The other (non-rand) i2c_item fields are not
read by any constraint and are left out of the harness.
"""
from dv_solve.problem import BIN_EQ, UN_INVERT

from tests.perf.svcorpus.case import inside_dist

NAME = "i2c_item"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/dv/sv/i2c_agent/i2c_item.sv", "lines": "36-37, 60-75"}
ABOUT = "a byte range and a read-dependent pairing of two flag bits, one under a conditional dist"

VARS = [("fbyte", 8, False), ("nakok", 1, False), ("rcont", 1, False),
        ("read", 1, False), ("stop", 1, False), ("start", 1, False)]
DIST = {"rcont": [(1, 1, 1, False), (0, 0, 2, False)]}
SOLVE_BEFORE = [(("read", "stop"), ("rcont",))]
DIST_WHEN = {"rcont": lambda m: not m["read"]}       # the dist is in the else branch


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    int_ = lambda k: b.expr_const(k, is_signed=True, width=32)   # noqa: E731
    b.add_constraint(b.expr_in_range(var("fbyte"), int_(0), int_(127)))
    b.add_constraint(b.expr_ite(var("read"),
                                b.expr_binary(BIN_EQ, var("rcont"),
                                              b.expr_unary(UN_INVERT, var("stop"))),
                                inside_dist(b, var("rcont"), DIST["rcont"])))
    b.add_constraint(b.expr_binary(BIN_EQ, int_(8), int_(8)))      # wait_cycles == 8
