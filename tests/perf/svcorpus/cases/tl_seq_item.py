"""OpenTitan tl_agent: a TileLink-UL request/response item (all its blocks).

    `define A_OPCODE_IS_VALID(a_opcode) (a_opcode inside {Get, PutFullData, PutPartialData})
    `define MASK_IS_FULL(a_mask, a_size) ($countones(a_mask) == (1 << a_size))
    `define MASK_LOW_IN_INACTIVE_LANES(a_mask, a_addr, a_size) \
        ((a_mask & ~(((1 << (1 << a_size)) - 1) << a_addr[SizeWidth-1:0])) == 0)
    `define ADDR_ALIGNED_TO_SIZE(a_addr, a_size) ((a_addr & ((1 << a_size) - 1)) == 0)
    `define BELOW_MAX_A_SIZE(a_size) (a_size <= 2)

    constraint param_c          { a_param == 0; d_param == 0; }
    constraint no_d_error_c     { soft d_error == 0; }
    constraint a_valid_len_c    { a_valid_len inside {[1:10]}; }
    constraint d_valid_len_c    { d_valid_len inside {[1:10]}; }
    constraint valid_delay_c    { a_valid_delay inside {[0:50]};
                                  d_valid_delay inside {[0:50]}; }
    constraint d_opcode_c       { d_opcode inside {AccessAckData, AccessAck}; }
    constraint a_opcode_c       { `A_OPCODE_IS_VALID(a_opcode); }
    constraint mask_contiguous_c {
      $countones(a_mask ^ {a_mask[MaskWidth-2:0], 1'b0}) <= 2; }
    constraint mask_w_PutFullData_c {
      a_opcode == PutFullData -> `MASK_IS_FULL(a_mask, a_size); }
    constraint mask_in_active_lanes_c { `MASK_LOW_IN_INACTIVE_LANES(a_mask, a_addr, a_size); }
    constraint addr_size_align_c { `ADDR_ALIGNED_TO_SIZE(a_addr, a_size); }
    constraint max_size_c        { `BELOW_MAX_A_SIZE(a_size); }

Widths (top_earlgrey via bus_params_pkg): AddrWidth = DataWidth = 32,
MaskWidth = 4, SizeWidth = 2, SourceWidth = 8, AUserWidth = 23,
DUserWidth = 14, OpcodeWidth = 3. a_opcode / d_opcode are plain bit [2:0]
(not enum-typed); tlul_pkg's enum literals are logic [2:0]: PutFullData = 0,
PutPartialData = 1, Get = 4; AccessAck = 0, AccessAckData = 1.
$countones (an int) is written as the sum of int'(x[i]) over the bits: the
builder's countones is a relation to a result variable, and an auxiliary
variable would be free in the equivalence check.
The free fields (a_data, a_source, a_user, d_data, d_source, d_size, d_user,
d_sink) are not in the capture.
"""
from dv_solve.problem import (BIN_ADD, BIN_BAND, BIN_BXOR, BIN_EQ, BIN_LSHIFT, BIN_LTE,
                              BIN_SUB, UN_INVERT)

from tests.perf.svcorpus.case import implies

NAME = "tl_seq_item"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/dv/sv/tl_agent/tl_seq_item.sv", "lines": "13-41, 42-160, 263-319"}
ABOUT = "TL-UL request: opcode sets, size cap, size-aligned address, mask contiguous / full / in active lanes"

VARS = [("a_addr", 32, False), ("a_data", 32, False), ("a_mask", 4, False),
        ("a_size", 2, False), ("a_param", 3, False), ("a_source", 8, False),
        ("a_opcode", 3, False), ("a_user", 23, False),
        ("d_param", 3, False), ("d_data", 32, False), ("d_source", 8, False),
        ("d_size", 2, False), ("d_opcode", 3, False), ("d_error", 1, False),
        ("d_user", 14, False), ("d_sink", 1, False),
        ("a_valid_delay", 32, False), ("a_valid_len", 32, False),
        ("d_valid_delay", 32, False), ("d_valid_len", 32, False)]
DIST = {}
SOLVE_BEFORE = []

PutFullData, PutPartialData, Get = 0, 1, 4
AccessAck, AccessAckData = 0, 1
SizeWidth, MaskWidth = 2, 4


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    int_ = lambda k: b.expr_const(k, is_signed=True, width=32)   # noqa: E731
    op3 = lambda k: b.expr_const(k, width=3)               # noqa: E731

    def countones(x, w):                                   # $countones(x), an int
        acc = b.expr_cast(b.expr_extract(x, 0, 0), 32, True)
        for i in range(1, w):
            acc = b.expr_binary(BIN_ADD, acc, b.expr_cast(b.expr_extract(x, i, i), 32, True))
        return acc

    def shl1(x):                                           # (1 << x)
        return b.expr_binary(BIN_LSHIFT, int_(1), x)

    addr, mask, size, opc = var("a_addr"), var("a_mask"), var("a_size"), var("a_opcode")
    # param_c
    b.add_constraint(b.expr_binary(BIN_EQ, var("a_param"), int_(0)))
    b.add_constraint(b.expr_binary(BIN_EQ, var("d_param"), int_(0)))
    # no_d_error_c
    b.add_soft_constraint(b.expr_binary(BIN_EQ, var("d_error"), int_(0)))
    # a_valid_len_c, d_valid_len_c, valid_delay_c
    b.add_constraint(b.expr_in_range(var("a_valid_len"), int_(1), int_(10)))
    b.add_constraint(b.expr_in_range(var("d_valid_len"), int_(1), int_(10)))
    b.add_constraint(b.expr_in_range(var("a_valid_delay"), int_(0), int_(50)))
    b.add_constraint(b.expr_in_range(var("d_valid_delay"), int_(0), int_(50)))
    # d_opcode_c, a_opcode_c
    b.add_constraint(b.expr_in_set(var("d_opcode"), [op3(AccessAckData), op3(AccessAck)]))
    b.add_constraint(b.expr_in_set(opc, [op3(Get), op3(PutFullData), op3(PutPartialData)]))
    # mask_contiguous_c
    shifted = b.expr_concat(b.expr_extract(mask, MaskWidth - 2, 0), b.expr_const(0, width=1), 1)
    b.add_constraint(b.expr_binary(BIN_LTE, countones(b.expr_binary(BIN_BXOR, mask, shifted),
                                                      MaskWidth), int_(2)))
    # mask_w_PutFullData_c
    b.add_constraint(implies(b, b.expr_binary(BIN_EQ, opc, op3(PutFullData)),
                             b.expr_binary(BIN_EQ, countones(mask, MaskWidth), shl1(size))))
    # mask_in_active_lanes_c
    lanes = b.expr_binary(BIN_LSHIFT, b.expr_binary(BIN_SUB, shl1(shl1(size)), int_(1)),
                          b.expr_extract(addr, SizeWidth - 1, 0))
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_binary(BIN_BAND, mask,
                                                         b.expr_unary(UN_INVERT, lanes)), int_(0)))
    # addr_size_align_c
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_binary(BIN_BAND, addr,
                                                         b.expr_binary(BIN_SUB, shl1(size), int_(1))),
                                   int_(0)))
    # max_size_c
    b.add_constraint(b.expr_binary(BIN_LTE, size, int_(2)))
