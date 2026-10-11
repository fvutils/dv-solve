"""OpenTitan keymgr: the base vseq's operation/OTP-key knobs.

    rand bit do_op_before_init;
    rand keymgr_pkg::keymgr_ops_e gen_operation;
    rand keymgr_pkg::keymgr_key_dest_e key_dest;
    rand bit do_rand_otp_key;
    rand bit do_invalid_otp_key;
    rand bit is_key_version_err;
    constraint is_key_version_err_c { is_key_version_err == 0; }
    constraint otp_key_c { do_rand_otp_key == 0; do_invalid_otp_key == 0; }
    constraint gen_operation_c {
      gen_operation inside {keymgr_pkg::OpGenId, keymgr_pkg::OpGenSwOut, keymgr_pkg::OpGenHwOut};
    }

keymgr_ops_e is logic [2:0] {OpAdvance=0, OpGenId=1, OpGenSwOut=2,
OpGenHwOut=3, OpDisable=4}; keymgr_key_dest_e is logic [1:0] {None, Aes,
Kmac, Otbn} (all four codes named). key_dest is in no constraint, so
Verilator leaves it out of the query; its membership is a tautology.
Left out: the `randomize() with` calls inside tasks (reseed_interval value
dist, key_version_val) -- inline constraints on CSR fields and locals, not
this class's constraint blocks.
"""
from dv_solve.problem import BIN_EQ

NAME = "keymgr_base"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/ip/keymgr/dv/env/seq_lib/keymgr_base_vseq.sv", "lines": "21-46"}
ABOUT = "three knob bits pinned to 0 and a generate-operation enum restricted to three values"

VARS = [("do_op_before_init", 1, False), ("gen_operation", 3, False), ("key_dest", 2, False),
        ("do_rand_otp_key", 1, False), ("do_invalid_otp_key", 1, False),
        ("is_key_version_err", 1, False)]
DIST = {}
SOLVE_BEFORE = []

OP_ADVANCE, OP_GEN_ID, OP_GEN_SW_OUT, OP_GEN_HW_OUT, OP_DISABLE = range(5)


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    i32 = lambda x: b.expr_const(x, is_signed=True, width=32)   # unsized literal
    op = lambda x: b.expr_const(x, width=3)                # keymgr_ops_e item  # noqa: E731

    # rand enums take only their named values
    b.add_constraint(b.expr_in_set(var("gen_operation"), [op(x) for x in range(5)]))
    b.add_constraint(b.expr_in_set(var("key_dest"), [b.expr_const(x, width=2) for x in range(4)]))

    b.add_constraint(b.expr_binary(BIN_EQ, var("is_key_version_err"), i32(0)))   # is_key_version_err_c
    b.add_constraint(b.expr_binary(BIN_EQ, var("do_rand_otp_key"), i32(0)))      # otp_key_c
    b.add_constraint(b.expr_binary(BIN_EQ, var("do_invalid_otp_key"), i32(0)))
    b.add_constraint(b.expr_in_set(var("gen_operation"),                          # gen_operation_c
                                   [op(OP_GEN_ID), op(OP_GEN_SW_OUT), op(OP_GEN_HW_OUT)]))
