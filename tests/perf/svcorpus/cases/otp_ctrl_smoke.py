"""OpenTitan otp_ctrl (top_earlgrey): the smoke vseq's DAI address within a partition.

    `define PART_CONTENT_RANGE(i) \\
        {[PartInfo[``i``].offset : (PartInfo[``i``].offset + PartInfo[``i``].size - DIGEST_SIZE - 1)]}
    rand bit access_locked_parts;
    rand bit [TL_DW-1:0] dai_addr;
    rand int num_dai_op;
    rand otp_ctrl_part_pkg::part_idx_e part_idx;
    rand bit check_regwen_val, check_trigger_regwen_val;
    rand bit [TL_DW-1:0] check_timeout_val;
    rand otp_ecc_err_e ecc_otp_err, ecc_chk_err;
    constraint no_access_err_c {access_locked_parts == 0;}
    constraint partition_index_c {part_idx inside {[0:LifeCycleIdx-1]};}
    constraint dai_wr_legal_addr_c {
      if (part_idx == VendorTestIdx)
          dai_addr inside `PART_CONTENT_RANGE(VendorTestIdx);
      ...                                 // the same for each of the 11 partitions
      if (part_idx == LifeCycleIdx)
          dai_addr inside `PART_CONTENT_RANGE(LifeCycleIdx);
      solve part_idx before dai_addr; }
    constraint dai_wr_blank_addr_c {
      dai_addr % 4 == 0;
      if (PartInfo[part_idx].secret) dai_addr % 8 == 0; }
    constraint num_trans_c {
      if (cfg.smoke_test) { num_trans == 1; num_dai_op inside {[1:2]}; }
      else { num_trans inside {[1:2]}; num_dai_op inside {[1:50]}; } }
    constraint regwens_c {
      check_regwen_val         dist {0 :/ 1, 1 :/ 9};
      check_trigger_regwen_val dist {0 :/ 1, 1 :/ 9}; }
    constraint check_timeout_val_c { check_timeout_val inside {0, [100_000:'1]}; }
    constraint ecc_otp_err_c {ecc_otp_err == OtpNoEccErr;}
    constraint ecc_chk_err_c {ecc_chk_err == OtpNoEccErr;}
    constraint apply_reset_during_pwr_init_cycles_c {
      apply_reset_during_pwr_init_cycles dist {
          [1:5] :/ 4, [6:2000] :/ 4, [2001:4000] :/ 2}; }

From the base classes: rand uint num_trans (dv_base_vseq; its num_trans_c is
overridden above) and rand int apply_reset_during_pwr_init_cycles
(otp_ctrl_base_vseq; its `== 0` block is overridden above). PartInfo is the
top_earlgrey table (offset/size are 11-bit, OtpByteAddrWidth), DIGEST_SIZE =
8 (`parameter uint`), TL_DW = 32; part_idx_e is a plain (int) enum of 15
names, otp_ecc_err_e a bit [1:0] enum of 3.

Deviations: cfg.smoke_test = 0 (dv_base_env_cfg default; only the dvsim
"smoke" mode passes +smoke_test=1). `PartInfo[part_idx].secret` is written
`(PartInfoSecret >> part_idx) & 1` over an 11-bit localparam of the .secret
fields: Verilator 5.052 lowers the struct-array lookup to a malformed
(select <bitvector> i) that z3 rejects (randomize fails), and the bit-select
`PartInfoSecret[part_idx]` to the bit at part_idx's CURRENT (pre-randomize)
value, a Verilator wrong-semantics bug. The class's rand vars in no
constraint (do_req_keys, do_lc_trans, rand_wr, rand_rd, rd_sw_tlul_rd,
wdata0, wdata1, check_trigger_val) are free in the case; Verilator draws them
outside the solver, so they are not in the query. The base classes' other
rand vars and blocks (otp_ctrl_base_vseq en_intr, cip_base_vseq
rand_reset_delay_c, csr_access_abort_pct_c) involve none of these: left out.
"""
from dv_solve.problem import BIN_ADD, BIN_AND, BIN_BAND, BIN_EQ, BIN_MOD, BIN_NEQ, BIN_RSHIFT, BIN_SUB

from tests.perf.svcorpus.case import implies

NAME = "otp_ctrl_smoke"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/top_earlgrey/ip_autogen/otp_ctrl/dv/env/seq_lib/otp_ctrl_smoke_vseq.sv",
          "lines": "5-89"}
ABOUT = "a DAI address picked per partition (11 ranges, 8-byte alignment for secret ones) plus knobs"

VARS = [("num_trans", 32, False), ("apply_reset_during_pwr_init_cycles", 32, True),
        ("do_req_keys", 1, False), ("do_lc_trans", 1, False),
        ("access_locked_parts", 1, False),
        ("rand_wr", 1, False), ("rand_rd", 1, False), ("rd_sw_tlul_rd", 1, False),
        ("dai_addr", 32, False), ("wdata0", 32, False), ("wdata1", 32, False),
        ("num_dai_op", 32, True), ("part_idx", 32, True),
        ("check_regwen_val", 1, False), ("check_trigger_regwen_val", 1, False),
        ("check_timeout_val", 32, False), ("check_trigger_val", 2, False),
        ("ecc_otp_err", 2, False), ("ecc_chk_err", 2, False)]
DIST = {"check_regwen_val": [(0, 0, 1, False), (1, 1, 9, False)],
        "check_trigger_regwen_val": [(0, 0, 1, False), (1, 1, 9, False)],
        "apply_reset_during_pwr_init_cycles": [(1, 5, 4, False), (6, 2000, 4, False),
                                               (2001, 4000, 2, False)]}
KNOWN_ISSUE = "B107: `inside` with a constant-expression bound ([0:LifeCycleIdx-1]) is CompileIncomplete"
SOLVE_BEFORE = [(("part_idx",), ("dai_addr",))]

CFG_SMOKE_TEST = 0
DIGEST_SIZE = 8
# PartInfo[i]: (offset, size, secret), top_earlgrey otp_ctrl_part_pkg
PART_INFO = [(0, 64, 0), (64, 368, 0), (432, 712, 0), (1144, 472, 0), (1616, 40, 0),
             (1656, 72, 0), (1728, 16, 0), (1744, 40, 1), (1784, 88, 1), (1872, 88, 1),
             (1960, 88, 0)]
LIFE_CYCLE_IDX = 10
NUM_PART_IDX_NAMES = 15                  # VendorTestIdx .. NumAgentsIdx
OTP_NO_ECC_ERR = 0


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    bin_ = b.expr_binary
    i32 = lambda x: b.expr_const(x, is_signed=True, width=32)   # noqa: E731  int / enum item
    eq = lambda a, c: bin_(BIN_EQ, a, c)                   # noqa: E731

    pidx, addr = var("part_idx"), var("dai_addr")
    # rand enums take only their named values
    b.add_constraint(b.expr_in_set(pidx, [i32(k) for k in range(NUM_PART_IDX_NAMES)]))
    for n in ("ecc_otp_err", "ecc_chk_err"):
        b.add_constraint(b.expr_in_set(var(n), [b.expr_const(k, width=2) for k in range(3)]))

    b.add_constraint(eq(var("access_locked_parts"), i32(0)))
    b.add_constraint(b.expr_in_range(pidx, i32(0), bin_(BIN_SUB, i32(LIFE_CYCLE_IDX), i32(1))))
    for i, (off, size, _) in enumerate(PART_INFO):
        o = lambda: b.expr_const(off, width=11)            # noqa: E731  PartInfo[i].offset
        hi = bin_(BIN_SUB, bin_(BIN_SUB, bin_(BIN_ADD, o(), b.expr_const(size, width=11)),
                                b.expr_const(DIGEST_SIZE, width=32)), i32(1))
        b.add_constraint(implies(b, eq(pidx, i32(i)), b.expr_in_range(addr, o(), hi)))
    b.add_constraint(eq(bin_(BIN_MOD, addr, i32(4)), i32(0)))
    secret = b.expr_const(sum(s << i for i, (_, _, s) in enumerate(PART_INFO)), width=11)
    b.add_constraint(implies(b, bin_(BIN_NEQ, bin_(BIN_BAND, bin_(BIN_RSHIFT, secret, pidx), i32(1)),
                                     i32(0)),
                             eq(bin_(BIN_MOD, addr, i32(8)), i32(0))))
    smoke = b.expr_const(CFG_SMOKE_TEST, width=1)
    nt, nd = var("num_trans"), var("num_dai_op")
    b.add_constraint(b.expr_ite(
        smoke,
        bin_(BIN_AND, eq(nt, i32(1)), b.expr_in_range(nd, i32(1), i32(2))),
        bin_(BIN_AND, b.expr_in_range(nt, i32(1), i32(2)), b.expr_in_range(nd, i32(1), i32(50)))))
    for n, es in DIST.items():                             # the dists' hard part
        b.add_constraint(b.expr_in_ranges(var(n), [(i32(lo), i32(hi)) for lo, hi, _, _ in es]))
    b.add_constraint(b.expr_in_ranges(var("check_timeout_val"),
                                      [(i32(0), i32(0)),
                                       (i32(100_000), b.expr_const(0xFFFF_FFFF, width=32))]))
    b.add_constraint(eq(var("ecc_otp_err"), b.expr_const(OTP_NO_ECC_ERR, width=2)))
    b.add_constraint(eq(var("ecc_chk_err"), b.expr_const(OTP_NO_ECC_ERR, width=2)))
