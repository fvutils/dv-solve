"""OpenTitan csrng agent: an application-interface command.

    rand acmd_e       acmd;        // enum logic [2:0]: INV..GENU = 0..7
    rand mubi4_t      flags;       // enum logic [3:0]: MuBi4True = 6, MuBi4False = 9
    rand bit [3:0]    clen;
    rand bit [11:0]   glen;
    rand bit [31:0]   cmd_data_q[$];
    constraint c_clen { clen inside {[0:12]}; }
    constraint c_cmd_data {
      solve clen before cmd_data_q;
      cmd_data_q.size() == clen;
    }
    constraint c_flags { flags inside {MuBi4True, MuBi4False}; }
    constraint c_glen {
      glen dist {
        // Note that 0 isn't supported by the implementation, see #23846.
        [1:32] :/ 75, [33:128] :/ 10, [129:1024] :/ 5, [1025:4094] :/ 5, 4095 := 5
      };
    }

The queue is a solver variable through its size, an `int` (Verilator names
it __Vcmd_data_q_size; the case uses that name so equiv.py can pair them),
never negative; its elements are unconstrained and drawn outside the solver.
The rand enums take only their named values: for acmd that is every 3-bit
value (Verilator leaves acmd out of the query), for flags {6, 9}.
`solve clen before cmd_data_q` is not in SOLVE_BEFORE: the size is a
function of clen, so the order cannot change the spread. BOX gives the
bench's reference sampler the size's range (it is equal to clen).
"""
from dv_solve.problem import BIN_EQ, BIN_GTE

from tests.perf.svcorpus.case import inside_dist

NAME = "csrng_item"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/dv/sv/csrng_agent/csrng_item.sv", "lines": "12-45"}
ABOUT = "two sparse enums, a queue size tied to a 4-bit length, and a 5-band glen dist"

VARS = [("acmd", 3, False), ("flags", 4, False), ("clen", 4, False), ("glen", 12, False),
        ("__Vcmd_data_q_size", 32, True)]
DIST = {"glen": [(1, 32, 75, False), (33, 128, 10, False), (129, 1024, 5, False),
                 (1025, 4094, 5, False), (4095, 4095, 5, True)]}
SOLVE_BEFORE = []                     # solve clen before cmd_data_q: see above
BOX = {"__Vcmd_data_q_size": (0, 12)}  # == clen; equiv.py proves it

ACMD = range(8)                       # INV, INS, RES, GEN, UPD, UNI, GENB, GENU
MuBi4True, MuBi4False = 0x6, 0x9


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    int_ = lambda k: b.expr_const(k, is_signed=True, width=32)   # noqa: E731
    # rand enums: one of their named values
    b.add_constraint(b.expr_in_set(var("acmd"), [b.expr_const(k, width=3) for k in ACMD]))
    mubi = [b.expr_const(MuBi4True, width=4), b.expr_const(MuBi4False, width=4)]
    b.add_constraint(b.expr_in_set(var("flags"), mubi))
    b.add_constraint(b.expr_in_range(var("clen"), int_(0), int_(12)))
    size = var("__Vcmd_data_q_size")
    b.add_constraint(b.expr_binary(BIN_EQ, size, var("clen")))
    b.add_constraint(b.expr_binary(BIN_GTE, size, int_(0)))      # implied: size() >= 0
    b.add_constraint(b.expr_in_set(var("flags"), mubi))          # c_flags
    b.add_constraint(inside_dist(b, var("glen"), DIST["glen"]))
