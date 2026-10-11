"""OpenTitan hmac: the base vseq's random write address/mask and key/digest config.

    rand bit [TL_AW-1:0]  wr_addr;      rand bit [TL_DBW-1:0] wr_mask;
    rand bit wr_config_during_hash, wr_key_during_hash, hmac_en;
    rand bit [3:0] digest_size;         rand bit [31:0] key[NUM_KEYS];
    rand bit [5:0] key_length;          rand bit endian_swap, digest_swap, key_swap;
    constraint hmac_base_vseq::wr_addr_c {
      wr_addr inside {[HMAC_MSG_FIFO_BASE : HMAC_MSG_FIFO_LAST_ADDR]}; }
    constraint hmac_base_vseq::wr_mask_c {
      $countones(wr_mask) dist { TL_DBW :/ 1, [1:TL_DBW-1] :/ 1 }; }
    constraint hmac_base_vseq::key_length_c {
      $countones(key_length) == 1 dist { 1 :/ 4, 0 :/ 1 }; }
    constraint hmac_base_vseq::digest_size_c {
      $countones(digest_size) == 1 dist { 1 :/ 4, 0 :/ 1 }; }
    constraint hmac_base_vseq::wr_mask_contiguous_c {
      $countones(wr_mask ^ {wr_mask[TL_DBW-2:0], 1'b0}) <= 2; }

TL_AW = TL_DW = 32, TL_DBW = 4, NUM_KEYS = 32 (uint), HMAC_MSG_FIFO_BASE =
32'h1000 and HMAC_MSG_FIFO_LAST_ADDR = BASE + 2048 - 1 = 32'h17FF (uint32 =
bit [31:0]). The inherited cip_base_vseq/dv_base_vseq constraints only
constrain their own fields, so they are left out; the 32 key words have no
constraint (the capture leaves them out of the query) but are rand fields,
declared here as key_0..key_31.

Deviations: Verilator 5.052 dies (internal error, DISTITEM reached the
emitter) on `$countones(x) == 1 dist {..}`; the harness writes it
`($countones(x) == 1) dist {..}`, the same parse. All three dists are on an
expression, not a variable, so there is nothing to give add_dist: DIST is
empty and the weights are lost; the hard part is kept (`== 1 dist {1, 0}`
always holds). The builder's countones (expr_countones) is a relation that
compiles only with a variable result and a variable operand; an int-valued
$countones(expr) here would need an auxiliary variable, which the capture
does not have, so build() writes $countones as the int sum of the operand's
bits.
"""
from dv_solve.problem import BIN_ADD, BIN_BXOR, BIN_EQ, BIN_LTE, BIN_OR

NAME = "hmac_base"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/ip/hmac/dv/env/seq_lib/hmac_base_vseq.sv", "lines": "20-30, 102-130"}
ABOUT = "message-FIFO address window, a contiguous byte mask by popcount, one-hot key/digest config"

TL_AW, TL_DBW, NUM_KEYS = 32, 4, 32
HMAC_MSG_FIFO_BASE = 0x1000
HMAC_MSG_FIFO_LAST_ADDR = HMAC_MSG_FIFO_BASE + 2048 - 1

VARS = ([("wr_addr", TL_AW, False), ("wr_mask", TL_DBW, False),
         ("wr_config_during_hash", 1, False), ("wr_key_during_hash", 1, False),
         ("hmac_en", 1, False), ("digest_size", 4, False)]
        + [(f"key_{i}", 32, False) for i in range(NUM_KEYS)]
        + [("key_length", 6, False), ("endian_swap", 1, False), ("digest_swap", 1, False),
           ("key_swap", 1, False)])
DIST = {}          # the three dists are on expressions; weights below for the record
EXPR_DIST = {"$countones(wr_mask)": [(TL_DBW, TL_DBW, 1, False), (1, TL_DBW - 1, 1, False)],
             "$countones(key_length) == 1": [(1, 1, 4, False), (0, 0, 1, False)],
             "$countones(digest_size) == 1": [(1, 1, 4, False), (0, 0, 1, False)]}
SOLVE_BEFORE = []


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    i32 = lambda x: b.expr_const(x, is_signed=True, width=32)   # unsized literal / int param
    u32 = lambda x: b.expr_const(x, width=32)              # uint32 param

    def countones(x, w):
        """$countones(x): an int, the sum of x's bits"""
        acc = None
        for i in range(w):
            bit = b.expr_extend(b.expr_extract(x, i, i), 1, 32)
            acc = bit if acc is None else b.expr_binary(BIN_ADD, acc, bit)
        return b.expr_cast(acc, 32, True)

    # wr_addr_c
    b.add_constraint(b.expr_in_range(var("wr_addr"), u32(HMAC_MSG_FIFO_BASE),
                                     u32(HMAC_MSG_FIFO_LAST_ADDR)))
    # wr_mask_c (hard part of the dist)
    m = var("wr_mask")
    pm = countones(m, TL_DBW)
    b.add_constraint(b.expr_binary(BIN_OR, b.expr_binary(BIN_EQ, pm, i32(TL_DBW)),
                                   b.expr_in_range(pm, i32(1), i32(TL_DBW - 1))))
    # key_length_c, digest_size_c (hard part of the dist: always holds)
    for n, w in (("key_length", 6), ("digest_size", 4)):
        one_hot = b.expr_binary(BIN_EQ, countones(var(n), w), i32(1))
        b.add_constraint(b.expr_in_set(one_hot, [i32(1), i32(0)]))
    # wr_mask_contiguous_c
    sh = b.expr_concat(b.expr_extract(m, TL_DBW - 2, 0), b.expr_const(0, width=1), 1)
    b.add_constraint(b.expr_binary(BIN_LTE, countones(b.expr_binary(BIN_BXOR, m, sh), TL_DBW),
                                   i32(2)))
