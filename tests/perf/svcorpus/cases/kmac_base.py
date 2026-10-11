"""OpenTitan kmac: the base vseq's hashing configuration (mode, strength, lengths, masks).

    rand bit kmac_en, xof_en, reg_en_sideload, entropy_timer_en, ...;
    rand kmac_pkg::key_len_e key_len;  rand sha3_pkg::sha3_mode_e hash_mode;
    rand sha3_pkg::keccak_strength_e strength;  rand kmac_pkg::entropy_mode_e entropy_mode;
    rand int unsigned output_len, keccak_block_size, fname_len, custom_str_len;
    rand byte fname_arr[], custom_str_arr[];
    rand bit [TL_AW-1:0] fifo_addr;  rand bit [TL_DBW-1:0] data_mask;
    rand kmac_pkg::err_code_e kmac_err_type;
    rand sha3_pkg::sha3_st_e err_sw_cmd_seq_st;  rand kmac_pkg::kmac_cmd_e err_sw_cmd_seq_cmd;
    rand bit [EntropyPeriodReserved-1:0] prescaler_val;            // 10 bits
    rand bit [TL_DW-KmacWaitTimer-1:0] entropy_wait_timer;         // 16 bits
    constraint xof_en_c { (!kmac_en) -> (xof_en == 1'b0); }
    constraint hash_mode_c { if (kmac_en) hash_mode == CShake; else hash_mode != CShake; }
    constraint output_len_sha3_c { if (hash_mode == Sha3) {
      (strength == L224) -> (output_len == 28); (strength == L256) -> (output_len == 32);
      (strength == L384) -> (output_len == 48); (strength == L512) -> (output_len == 64); } }
    constraint strength_c {
      solve kmac_err_type before hash_mode; solve kmac_err_type before strength;
      if (kmac_err_type == ErrUnexpectedModeStrength) {
        (hash_mode inside {Shake, CShake}) -> !(strength inside {L128, L256});
        (hash_mode == Sha3) -> (strength == L128);
      } else {
        (hash_mode inside {Shake, CShake}) -> (strength inside {L128, L256});
        (hash_mode == Sha3) -> (strength != L128); } }
    constraint key_len_c { (reg_en_sideload) -> key_len == Key256; }
    constraint entropy_mode_c {
      if (kmac_err_type == ErrIncorrectEntropyMode)
        !(entropy_mode inside {EntropyModeSw, EntropyModeEdn});
      else entropy_mode == static_entropy_mode; }
    constraint keccak_block_size_c {
      (strength == L128) -> (keccak_block_size == 168); ... L224 144, L256 136,
      L384 104, L512 72 }
    constraint err_sw_cmd_seq_c {
      solve err_sw_cmd_seq_st before err_sw_cmd_seq_cmd;
      if (kmac_err_type == ErrSwCmdSequence) {
        if (err_sw_cmd_seq_st == StIdle) cmd inside {CmdProcess, CmdManualRun, CmdDone};
        else if (st == StAbsorb) cmd inside {CmdStart, CmdManualRun, CmdDone};
        else if (st == StSqueeze) cmd inside {CmdStart, CmdProcess};
        else if (st inside {StManualRun, StFlush}) cmd != CmdNone; } }
    constraint fifo_addr_c { fifo_addr inside {[KMAC_FIFO_BASE : KMAC_FIFO_END]}; }
    constraint data_mask_contiguous_c {
      $countones(data_mask ^ {data_mask[TL_DBW-2:0], 1'b0}) <= 2; }
    constraint data_mask_c { $countones(data_mask) dist { [1 : TL_DBW] :/ 1, TL_DBW :/ 1 }; }
    constraint prefix_len_c { fname_len + custom_str_len <= 36;
      fname_arr.size() == fname_len; custom_str_arr.size() == custom_str_len; }
    constraint prefix_is_char_c { foreach (fname_arr[i]) fname_arr[i] inside {32, [65:90], [97:122]};
                                  foreach (custom_str_arr[i]) ... the same }
    constraint entropy_period_c {
      if (entropy_mode == EntropyModeEdn && cfg.enable_masking) {
        if (entropy_timer_en)
          (prescaler_val + 1) * entropy_wait_timer >
            (cfg.m_edn_pull_agent_cfgs[0].device_delay_max * 4 + 10) *
            (cfg.edn_clk_freq_mhz / cfg.clk_freq_mhz + 1);
        else entropy_wait_timer == 0; }
      solve entropy_mode before prescaler_val, entropy_wait_timer; }

TL_DW = TL_AW = 32, TL_DBW = 4 (localparam int), KMAC_FIFO_BASE/END =
32'h800/32'hFFC; enum encodings from sha3_pkg / kmac_pkg (see the constants).

Constants for non-rand state: static_entropy_mode = EntropyModeEdn (declared
Sw, but pre_start randomizes it inside {Sw, Edn}; Edn makes entropy_period_c
live); cfg.enable_masking = 1 (kmac_env_cfg default); device_delay_max = 100,
edn_clk_freq_mhz = clk_freq_mhz = 100 (all rand in the cfg; typical values:
the default ClkFreqDiffNone makes the two clocks equal), so the bound is 820.

Dynamic arrays: Verilator solves a `.size()` in the first query as its own
int variable (__V<name>_size, >= 0) and the elements in a later query once
the size is drawn. The case declares the two size variables under those names
(SV's implicit `fname_arr.size()`; the names map them to the capture) and
leaves the elements and prefix_is_char_c out: they are not in the first
query. `$countones(data_mask) dist` is on an expression: its weights are in
EXPR_DIST for the record (add_dist takes a variable), the hard part is kept.
$countones is the int sum of the operand's bits, as in tl_seq_item. The free
rand vars (enable_intr, msg_endian, state_endian, provide_sideload_key,
entropy_fast_process, entropy_ready, en_unsupported_modestrength, msg_mask,
hash_threshold, hash_cnt_clr, entropy_req) are not in the query; the case
keeps them free. key_share (a 1024-bit packed rand, in no constraint) and
msg[] (a rand dynamic array with no size constraint here: stays empty) are
left out.
"""
from dv_solve.problem import (BIN_ADD, BIN_AND, BIN_BXOR, BIN_DIV, BIN_EQ, BIN_GT, BIN_GTE,
                              BIN_LTE, BIN_MUL, BIN_NEQ, UN_NOT)

from tests.perf.svcorpus.case import all_of, implies

NAME = "kmac_base"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/ip/kmac/dv/env/seq_lib/kmac_base_vseq.sv", "lines": "18-276"}
ABOUT = "SHA3/SHAKE/cSHAKE mode x strength x error-type tables, lengths, a contiguous byte mask"

VARS = [("enable_intr", 32, False), ("kmac_en", 1, False), ("xof_en", 1, False),
        ("key_len", 3, False), ("hash_mode", 2, False), ("strength", 3, False),
        ("msg_endian", 1, False), ("state_endian", 1, False),
        ("provide_sideload_key", 1, False), ("reg_en_sideload", 1, False),
        ("entropy_mode", 2, False), ("entropy_fast_process", 1, False),
        ("entropy_ready", 1, False), ("en_unsupported_modestrength", 1, False),
        ("msg_mask", 1, False), ("output_len", 32, False), ("keccak_block_size", 32, False),
        ("fname_len", 32, False), ("custom_str_len", 32, False),
        ("__Vfname_arr_size", 32, True), ("__Vcustom_str_arr_size", 32, True),
        ("fifo_addr", 32, False), ("data_mask", 4, False), ("kmac_err_type", 8, False),
        ("err_sw_cmd_seq_st", 3, False), ("err_sw_cmd_seq_cmd", 6, False),
        ("hash_threshold", 10, False), ("hash_cnt_clr", 1, False), ("entropy_req", 1, False),
        ("entropy_timer_en", 1, False), ("prescaler_val", 10, False),
        ("entropy_wait_timer", 16, False)]
DIST = {}
EXPR_DIST = {"$countones(data_mask)": [(1, 4, 1, False), (4, 4, 1, False)]}
SOLVE_BEFORE = [(("kmac_err_type",), ("hash_mode",)), (("kmac_err_type",), ("strength",)),
                (("err_sw_cmd_seq_st",), ("err_sw_cmd_seq_cmd",)),
                (("entropy_mode",), ("prescaler_val", "entropy_wait_timer"))]

TL_DBW = 4
KMAC_FIFO_BASE, KMAC_FIFO_END = 0x800, 0xFFC
STATIC_ENTROPY_MODE = 1                                    # EntropyModeEdn
CFG_ENABLE_MASKING = 1
CFG_DEVICE_DELAY_MAX, CFG_EDN_CLK_FREQ_MHZ, CFG_CLK_FREQ_MHZ = 100, 100, 100

SHA3, SHAKE, CSHAKE = 0b00, 0b10, 0b11                     # sha3_mode_e, 2 bits
L128, L224, L256, L384, L512 = 0, 1, 2, 3, 4               # keccak_strength_e, 3 bits
KEY_LENS = (0, 1, 2, 3, 4)                                 # key_len_e, 3 bits; Key256 = 2
KEY256 = 2
ENTROPY_NONE, ENTROPY_EDN, ENTROPY_SW = 0, 1, 2            # entropy_mode_e, 2 bits
ERR_CODES = (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xC1, 0xC2, 0xC3)   # err_code_e, 8 bits
ERR_INCORRECT_ENTROPY_MODE, ERR_UNEXPECTED_MODE_STRENGTH, ERR_SW_CMD_SEQUENCE = 5, 6, 8
ST_IDLE, ST_ABSORB, ST_SQUEEZE, ST_MANUAL_RUN, ST_FLUSH, ST_ERROR = range(6)   # sha3_st_e
CMD_NONE, CMD_START, CMD_PROCESS, CMD_MANUAL_RUN, CMD_DONE = 0, 29, 46, 49, 22  # kmac_cmd_e


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    bin_ = b.expr_binary
    i32 = lambda x: b.expr_const(x, is_signed=True, width=32)   # noqa: E731  unsized / int
    u32 = lambda x: b.expr_const(x, width=32)              # noqa: E731  int unsigned / bit[31:0]
    eq = lambda a, c: bin_(BIN_EQ, a, c)                   # noqa: E731
    neg = lambda a: b.expr_unary(UN_NOT, a)                # noqa: E731
    mode = lambda k: b.expr_const(k, width=2)              # noqa: E731
    stren = lambda k: b.expr_const(k, width=3)             # noqa: E731
    err = lambda k: b.expr_const(k, width=8)               # noqa: E731
    st = lambda k: b.expr_const(k, width=3)                # noqa: E731
    cmd = lambda k: b.expr_const(k, width=6)               # noqa: E731
    emode = lambda k: b.expr_const(k, width=2)             # noqa: E731

    def countones(x, w):                                   # $countones(x), an int
        acc = b.expr_cast(b.expr_extract(x, 0, 0), 32, True)
        for i in range(1, w):
            acc = bin_(BIN_ADD, acc, b.expr_cast(b.expr_extract(x, i, i), 32, True))
        return acc

    hm, s, et = var("hash_mode"), var("strength"), var("kmac_err_type")
    # rand enums take only their named values
    for n, mk, vals in (("key_len", stren, KEY_LENS), ("hash_mode", mode, (SHA3, SHAKE, CSHAKE)),
                        ("strength", stren, (L128, L224, L256, L384, L512)),
                        ("entropy_mode", emode, (ENTROPY_NONE, ENTROPY_EDN, ENTROPY_SW)),
                        ("kmac_err_type", err, ERR_CODES),
                        ("err_sw_cmd_seq_st", st, range(6)),
                        ("err_sw_cmd_seq_cmd", cmd, (CMD_NONE, CMD_START, CMD_PROCESS,
                                                     CMD_MANUAL_RUN, CMD_DONE))):
        b.add_constraint(b.expr_in_set(var(n), [mk(k) for k in vals]))

    # xof_en_c
    b.add_constraint(implies(b, neg(var("kmac_en")), eq(var("xof_en"), b.expr_const(0, width=1))))
    # hash_mode_c
    b.add_constraint(b.expr_ite(var("kmac_en"), eq(hm, mode(CSHAKE)),
                                bin_(BIN_NEQ, hm, mode(CSHAKE))))
    # output_len_sha3_c
    ol = var("output_len")
    b.add_constraint(implies(b, eq(hm, mode(SHA3)), all_of(b, *[
        implies(b, eq(s, stren(L224)), eq(ol, i32(28))),
        implies(b, eq(s, stren(L256)), eq(ol, i32(32))),
        implies(b, eq(s, stren(L384)), eq(ol, i32(48))),
        implies(b, eq(s, stren(L512)), eq(ol, i32(64)))])))
    # strength_c
    shake = lambda: b.expr_in_set(hm, [mode(SHAKE), mode(CSHAKE)])        # noqa: E731
    xof_strength = lambda: b.expr_in_set(s, [stren(L128), stren(L256)])   # noqa: E731
    b.add_constraint(b.expr_ite(
        eq(et, err(ERR_UNEXPECTED_MODE_STRENGTH)),
        bin_(BIN_AND, implies(b, shake(), neg(xof_strength())),
             implies(b, eq(hm, mode(SHA3)), eq(s, stren(L128)))),
        bin_(BIN_AND, implies(b, shake(), xof_strength()),
             implies(b, eq(hm, mode(SHA3)), bin_(BIN_NEQ, s, stren(L128))))))
    # key_len_c
    b.add_constraint(implies(b, var("reg_en_sideload"), eq(var("key_len"), stren(KEY256))))
    # entropy_mode_c
    em = var("entropy_mode")
    b.add_constraint(b.expr_ite(eq(et, err(ERR_INCORRECT_ENTROPY_MODE)),
                                neg(b.expr_in_set(em, [emode(ENTROPY_SW), emode(ENTROPY_EDN)])),
                                eq(em, emode(STATIC_ENTROPY_MODE))))
    # keccak_block_size_c
    for k, size in ((L128, 168), (L224, 144), (L256, 136), (L384, 104), (L512, 72)):
        b.add_constraint(implies(b, eq(s, stren(k)), eq(var("keccak_block_size"), i32(size))))
    # err_sw_cmd_seq_c
    sst, scmd = var("err_sw_cmd_seq_st"), var("err_sw_cmd_seq_cmd")
    cmds = lambda *ks: b.expr_in_set(scmd, [cmd(k) for k in ks])          # noqa: E731
    b.add_constraint(implies(b, eq(et, err(ERR_SW_CMD_SEQUENCE)), b.expr_ite(
        eq(sst, st(ST_IDLE)), cmds(CMD_PROCESS, CMD_MANUAL_RUN, CMD_DONE),
        b.expr_ite(eq(sst, st(ST_ABSORB)), cmds(CMD_START, CMD_MANUAL_RUN, CMD_DONE),
                   b.expr_ite(eq(sst, st(ST_SQUEEZE)), cmds(CMD_START, CMD_PROCESS),
                              implies(b, b.expr_in_set(sst, [st(ST_MANUAL_RUN), st(ST_FLUSH)]),
                                      bin_(BIN_NEQ, scmd, cmd(CMD_NONE))))))))
    # fifo_addr_c
    b.add_constraint(b.expr_in_range(var("fifo_addr"), u32(KMAC_FIFO_BASE), u32(KMAC_FIFO_END)))
    # data_mask_contiguous_c
    dm = var("data_mask")
    sh = b.expr_concat(b.expr_extract(dm, TL_DBW - 2, 0), b.expr_const(0, width=1), 1)
    b.add_constraint(bin_(BIN_LTE, countones(bin_(BIN_BXOR, dm, sh), TL_DBW), i32(2)))
    # data_mask_c (hard part of the dist)
    b.add_constraint(b.expr_in_ranges(countones(dm, TL_DBW),
                                      [(i32(lo), i32(hi)) for lo, hi, _, _
                                       in EXPR_DIST["$countones(data_mask)"]]))
    # prefix_len_c
    b.add_constraint(bin_(BIN_LTE, bin_(BIN_ADD, var("fname_len"), var("custom_str_len")), i32(36)))
    for arr, ln in (("fname_arr", "fname_len"), ("custom_str_arr", "custom_str_len")):
        size = var(f"__V{arr}_size")                       # arr.size(), an int
        b.add_constraint(bin_(BIN_GTE, size, i32(0)))
        b.add_constraint(eq(size, var(ln)))
    # entropy_period_c
    bound = bin_(BIN_MUL,
                 bin_(BIN_ADD, bin_(BIN_MUL, u32(CFG_DEVICE_DELAY_MAX), i32(4)), i32(10)),
                 bin_(BIN_ADD, bin_(BIN_DIV, u32(CFG_EDN_CLK_FREQ_MHZ), u32(CFG_CLK_FREQ_MHZ)),
                      i32(1)))
    wt = var("entropy_wait_timer")
    b.add_constraint(implies(
        b, bin_(BIN_AND, eq(em, emode(ENTROPY_EDN)), b.expr_const(CFG_ENABLE_MASKING, width=1)),
        b.expr_ite(var("entropy_timer_en"),
                   bin_(BIN_GT, bin_(BIN_MUL, bin_(BIN_ADD, var("prescaler_val"), i32(1)), wt),
                        bound),
                   eq(wt, i32(0)))))
