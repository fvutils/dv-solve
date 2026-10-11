"""OpenTitan flash_ctrl: an on-the-fly flash op (partition, address, size) and the MP regions.

    rand int ctrl_data_num, ctrl_info_num, ctrl_num;  rand bit is_addr_odd;
    rand int fractions;  rand flash_op_t rand_op;
    rand bit [2:0] allow_spec_info_acc;  rand bit all_entry_en;
    rand otf_cfg_mode_e scr_ecc_cfg;
    rand flash_mp_region_cfg_t rand_regions[MpRegions];
    rand flash_bank_mp_info_page_cfg_t rand_info[NumBanks][InfoTypes][$];
    constraint all_ent_c {
      solve all_entry_en before rand_regions, rand_info;
      if (cfg.en_always_any) all_entry_en == 1;
      else all_entry_en dist { 1 := 1, 0 := 4}; }
    constraint scr_ecc_c { scr_ecc_cfg dist { OTFCfgRand := 5, OTFCfgTrue := 4}; }
    constraint rand_regions_c { foreach (rand_regions[i]) {
      if (all_entry_en) rand_regions[i].en == MuBi4True;
      rand_regions[i].start_page dist { 0 := 1, [1 : FlashNumPages - 2] :/ 8,
                                        FlashNumPages - 1 := 1 };
      rand_regions[i].num_pages inside {[1 : FlashNumPages - rand_regions[i].start_page]};
      rand_regions[i].num_pages <= 32;
      rand_regions[i].scramble_en dist { MuBi4True := 4, MuBi4False := 1};
      rand_regions[i].ecc_en dist { MuBi4True := 4, MuBi4False := 1}; } }
    constraint rand_info_c { ... }                              (left out, see below)
    constraint ctrl_data_num_c {
      ctrl_data_num dist { CtrlTransMin := 2, [2:31] :/ 1, CtrlTransMax := 2}; }
    constraint fractions_c {
      if (cfg.seq_cfg.trigger_prog_res_fault) fractions inside {[17:32]};
      else fractions dist { [1:4] := 4, [5:15] := 1, 16 := 1};
      cfg.seq_cfg.addr_flash_word_aligned -> fractions[0] == 1'b0; }
    constraint ctrl_info_num_c {
      solve rand_op before ctrl_info_num;
      ctrl_info_num inside {[1 : InfoTypeSize[rand_op.partition >> 1]]};
      if (cfg.ecc_mode > FlashEccEnabled) ctrl_info_num * fractions <= 128; }
    constraint ctrl_num_c {
      solve ctrl_data_num, ctrl_info_num, rand_op before ctrl_num;
      if (rand_op.partition == FlashPartData) ctrl_num == ctrl_data_num;
      else ctrl_num == ctrl_info_num; }
    constraint rand_op_c {
      solve fractions before rand_op.addr;  solve flash_program_data before rand_op;
      solve rand_op.partition before rand_op.prog_sel, rand_op.addr;
      solve rand_op.addr before rand_op.otf_addr;  solve rand_op.addr before rand_op.num_words;
      if (cfg.seq_cfg.op_readonly_on_info_partition) {
        if (cfg.seq_cfg.avoid_ro_partitions) rand_op.partition != FlashPartInfo;
        else rand_op.partition == FlashPartInfo -> rand_op.op == FlashOpRead; }
      if (cfg.seq_cfg.op_readonly_on_info1_partition) { ... the same for FlashPartInfo1 ... }
      rand_op.partition dist { FlashPartData := 6, FlashPartInfo := 1, FlashPartInfo1 := 1,
                               FlashPartInfo2 := 1 };
      rand_op.addr[TL_AW-1:BusAddrByteW] == 'h0;
      rand_op.addr[1:0] == 'h0;
      cfg.seq_cfg.addr_flash_word_aligned -> rand_op.addr[2] == 1'b0;
      if (fractions == 16) rand_op.addr[2] == 0;
      if (rand_op.partition != FlashPartData) {
        rand_op.addr inside
          {[0:InfoTypeBytes[rand_op.partition>>1]-(FlashBankBytesPerWord*fractions)]};
        rand_op.prog_sel == 1;
      } else rand_op.prog_sel == 0;
      if (cfg.ecc_mode > FlashEccEnabled) {
        if (rand_op.partition == FlashPartData)
          rand_op.addr[18:17] == cfg.tgt_pre[rand_op.partition][cfg.seq_cfg.ecc_err_target];
        else rand_op.addr[10:9] == cfg.tgt_pre[rand_op.partition][cfg.seq_cfg.ecc_err_target]; }
      rand_op.otf_addr == rand_op.addr[BusAddrByteW-2:0];
      rand_op.num_words == fractions; }
    constraint special_info_acc_c { allow_spec_info_acc dist { 3'h7 := 1, 3'h0 := 1, [1:6] :/ 2}; }

Earlgrey parameters: TL_AW = 32, BusAddrByteW = 20, MpRegions = 8,
FlashNumPages = 512, InfoTypeSize = '{10, 1, 2} (int), InfoTypeBytes =
'{20480, 2048, 4096} (uint), FlashBankBytesPerWord = 8 (uint), CtrlTransMin/
Max = 1/32 (int unsigned). flash_op_t and flash_mp_region_cfg_t are packed
structs, one vector each, as in the capture: rand_op is 91 bits {partition
[90:87] (flash_dv_part_e, logic [3:0]: 0/1/2/4), erase_type [86], op [85:84],
prog_sel [83], num_words [82:51], addr [50:19], otf_addr [18:0]};
rand_regions[i] is 92 bits {en [91:88], read_en, program_en, erase_en,
scramble_en [75:72], ecc_en [71:68], he_en [67:64] (mubi4_t), num_pages
[63:32], start_page [31:0]}. Verilator gives packed-struct enum members no
membership constraint, and neither does the case.

Non-rand state as constants (defaults): cfg.en_always_any = 0, cfg.ecc_mode =
FlashEccDisabled (so the ecc clauses are vacuous); cfg.seq_cfg:
trigger_prog_res_fault = 0, addr_flash_word_aligned = 0, avoid_ro_partitions
= 0, op_readonly_on_info_partition = 0, op_readonly_on_info1_partition = 1
(the configure() defaults without plusargs), ecc_err_target = TgtRd;
cfg.tgt_pre[p][i] = i, so tgt_pre[p][TgtRd] is 2'b0 for every partition.

Left out: rand_info (a rand queue per bank/info type, sized by rand_info_c;
its elements only carry mubi dists) and rand_info_c; flash_program_data
(the base vseq's rand queue, here only in a solve-before). Harness changes
for Verilator 5.052: the three solve-befores between members of rand_op are
dropped (Verilator reads a packed-struct member as the whole struct, making
them `rand_op before rand_op`: circular, randomize() fails); the parameter
arrays InfoTypeSize/InfoTypeBytes indexed by `rand_op.partition >> 1` are
written as a ?: over the index (Verilator emits the indexed read as the
malformed `(select #x00000007 i)`), with 0 out of bounds as SV reads it,
which is also how build() writes the constant-array read; the ecc block
reading cfg.tgt_pre (an associative array indexed by a rand enum, emitted as
a malformed select even under its constant-false guard) is dropped from the
harness and kept here. The dists on struct members (partition, start_page,
scramble_en, ecc_en) have no variable to give add_dist: their hard part is
kept, their weights are in EXPR_DIST for the record.
"""
from dv_solve.problem import (BIN_AND, BIN_EQ, BIN_GT, BIN_LTE, BIN_MUL, BIN_NEQ, BIN_OR,
                              BIN_RSHIFT, BIN_SUB, UN_NOT)

from tests.perf.svcorpus.case import implies

NAME = "flash_ctrl_otf_base"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/ip_templates/flash_ctrl/dv/env/seq_lib/flash_ctrl_otf_base_vseq.sv",
          "lines": "24-158"}
ABOUT = "flash op: partition-dependent address window and size, op count by partition, 8 MP regions"

TL_AW, BUS_ADDR_BYTE_W, MP_REGIONS = 32, 20, 8
FLASH_NUM_PAGES = 512
INFO_TYPE_SIZE = [10, 1, 2]
INFO_TYPE_BYTES = [20480, 2048, 4096]
FLASH_BANK_BYTES_PER_WORD = 8
CTRL_TRANS_MIN, CTRL_TRANS_MAX = 1, 32
MUBI4_TRUE, MUBI4_FALSE = 0x6, 0x9
OTF_CFG_TRUE, OTF_CFG_FALSE, OTF_CFG_RAND = 0, 1, 2
PART_DATA, PART_INFO, PART_INFO1, PART_INFO2 = 0, 1, 2, 4
FLASH_OP_READ = 0
FLASH_ECC_DISABLED, FLASH_ECC_ENABLED = 0, 1

EN_ALWAYS_ANY = 0
ECC_MODE = FLASH_ECC_DISABLED
TRIGGER_PROG_RES_FAULT = 0
ADDR_FLASH_WORD_ALIGNED = 0
AVOID_RO_PARTITIONS = 0
OP_READONLY_ON_INFO_PARTITION = 0
OP_READONLY_ON_INFO1_PARTITION = 1
TGT_PRE_RD = 0                     # cfg.tgt_pre[p][TgtRd], every partition

VARS = ([("ctrl_data_num", 32, True), ("ctrl_info_num", 32, True), ("ctrl_num", 32, True),
         ("is_addr_odd", 1, False), ("fractions", 32, True), ("rand_op", 91, False),
         ("allow_spec_info_acc", 3, False), ("all_entry_en", 1, False),
         ("scr_ecc_cfg", 2, False)]
        + [(f"rand_regions[{i}]", 92, False) for i in range(MP_REGIONS)])
DIST = {"all_entry_en": [(1, 1, 1, True), (0, 0, 4, True)],
        "scr_ecc_cfg": [(OTF_CFG_RAND, OTF_CFG_RAND, 5, True),
                        (OTF_CFG_TRUE, OTF_CFG_TRUE, 4, True)],
        "ctrl_data_num": [(CTRL_TRANS_MIN, CTRL_TRANS_MIN, 2, True), (2, 31, 1, False),
                          (CTRL_TRANS_MAX, CTRL_TRANS_MAX, 2, True)],
        "fractions": [(1, 4, 4, True), (5, 15, 1, True), (16, 16, 1, True)],
        "allow_spec_info_acc": [(7, 7, 1, True), (0, 0, 1, True), (1, 6, 2, False)]}
_MUBI_DIST = [(MUBI4_TRUE, MUBI4_TRUE, 4, True), (MUBI4_FALSE, MUBI4_FALSE, 1, True)]
EXPR_DIST = {"rand_op.partition": [(PART_DATA, PART_DATA, 6, True), (PART_INFO, PART_INFO, 1, True),
                                   (PART_INFO1, PART_INFO1, 1, True),
                                   (PART_INFO2, PART_INFO2, 1, True)],
             "rand_regions[i].start_page": [(0, 0, 1, True), (1, FLASH_NUM_PAGES - 2, 8, False),
                                            (FLASH_NUM_PAGES - 1, FLASH_NUM_PAGES - 1, 1, True)],
             "rand_regions[i].scramble_en": _MUBI_DIST, "rand_regions[i].ecc_en": _MUBI_DIST}
SOLVE_BEFORE = [(("all_entry_en",), tuple(f"rand_regions[{i}]" for i in range(MP_REGIONS))),
                (("rand_op",), ("ctrl_info_num",)),
                (("ctrl_data_num", "ctrl_info_num", "rand_op"), ("ctrl_num",)),
                (("fractions",), ("rand_op",))]       # as written: before rand_op.addr
KNOWN_ISSUE = "B106: variables wider than 64 bits (packed structs) are unsupported on the builder path"
# solve-befores between members of rand_op (one variable here), for the record
SOLVE_BEFORE_MEMBERS = [(("fractions",), ("rand_op.addr",)),
                        (("rand_op.partition",), ("rand_op.prog_sel", "rand_op.addr")),
                        (("rand_op.addr",), ("rand_op.otf_addr",)),
                        (("rand_op.addr",), ("rand_op.num_words",))]


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    i32 = lambda x: b.expr_const(x, is_signed=True, width=32)   # unsized literal / int param
    u32 = lambda x: b.expr_const(x, width=32)              # uint param, 'h literal
    k = lambda x, w: b.expr_const(x, width=w)              # sized literal / enum item  # noqa: E731
    bit = lambda x: b.expr_const(x, width=1)
    eq = lambda a, c: b.expr_binary(BIN_EQ, a, c)          # noqa: E731
    neg = lambda a: b.expr_unary(UN_NOT, a)                # noqa: E731
    land = lambda a, c: b.expr_binary(BIN_AND, a, c)       # noqa: E731
    cond = lambda c, x: b.add_constraint(implies(b, c, x))  # noqa: E731
    ifelse = lambda c, x, y: (cond(c, x), cond(neg(c), y))  # noqa: E731
    ext = b.expr_extract

    def const_array(idx, vals, mk):
        """vals[idx] of a constant array: 0 out of bounds"""
        acc = mk(0)
        for j in reversed(range(len(vals))):
            acc = b.expr_ite(eq(idx, i32(j)), mk(vals[j]), acc)
        return acc

    op = var("rand_op")
    partition, rop, prog_sel = ext(op, 90, 87), ext(op, 85, 84), ext(op, 83, 83)
    num_words, addr, otf_addr = ext(op, 82, 51), ext(op, 50, 19), ext(op, 18, 0)
    fractions, all_entry_en = var("fractions"), var("all_entry_en")
    ecc_on = b.expr_binary(BIN_GT, i32(ECC_MODE), i32(FLASH_ECC_ENABLED))

    # rand enums take only their named values (scr_ecc_cfg; Verilator gives packed-struct
    # members none)
    b.add_constraint(b.expr_in_set(var("scr_ecc_cfg"),
                                   [k(x, 2) for x in (OTF_CFG_TRUE, OTF_CFG_FALSE, OTF_CFG_RAND)]))
    # all_ent_c
    ifelse(bit(EN_ALWAYS_ANY), eq(all_entry_en, i32(1)),
           b.expr_in_set(all_entry_en, [i32(1), i32(0)]))
    # scr_ecc_c
    b.add_constraint(b.expr_in_set(var("scr_ecc_cfg"), [k(OTF_CFG_RAND, 2), k(OTF_CFG_TRUE, 2)]))
    # rand_regions_c
    for i in range(MP_REGIONS):
        r = var(f"rand_regions[{i}]")
        en, scr, ecc = ext(r, 91, 88), ext(r, 75, 72), ext(r, 71, 68)
        num_pages, start_page = ext(r, 63, 32), ext(r, 31, 0)
        cond(all_entry_en, eq(en, k(MUBI4_TRUE, 4)))
        b.add_constraint(b.expr_binary(BIN_OR, b.expr_in_set(start_page, [i32(0),
                                                                        u32(FLASH_NUM_PAGES - 1)]),
                                       b.expr_in_range(start_page, i32(1),
                                                       u32(FLASH_NUM_PAGES - 2))))
        b.add_constraint(b.expr_in_range(num_pages, i32(1),
                                         b.expr_binary(BIN_SUB, u32(FLASH_NUM_PAGES), start_page)))
        b.add_constraint(b.expr_binary(BIN_LTE, num_pages, i32(32)))
        for f in (scr, ecc):
            b.add_constraint(b.expr_in_set(f, [k(MUBI4_TRUE, 4), k(MUBI4_FALSE, 4)]))
    # ctrl_data_num_c
    b.add_constraint(b.expr_binary(BIN_OR, b.expr_in_set(var("ctrl_data_num"),
                                                      [u32(CTRL_TRANS_MIN), u32(CTRL_TRANS_MAX)]),
                                   b.expr_in_range(var("ctrl_data_num"), i32(2), i32(31))))
    # fractions_c
    ifelse(bit(TRIGGER_PROG_RES_FAULT), b.expr_in_range(fractions, i32(17), i32(32)),
           b.expr_in_ranges(fractions, [(i32(1), i32(4)), (i32(5), i32(15)), (i32(16), i32(16))]))
    cond(bit(ADDR_FLASH_WORD_ALIGNED), eq(ext(fractions, 0, 0), k(0, 1)))
    # ctrl_info_num_c
    pidx = b.expr_binary(BIN_RSHIFT, partition, i32(1))
    b.add_constraint(b.expr_in_range(var("ctrl_info_num"), i32(1),
                                     const_array(pidx, INFO_TYPE_SIZE, i32)))
    cond(ecc_on, b.expr_binary(BIN_LTE, b.expr_binary(BIN_MUL, var("ctrl_info_num"), fractions),
                               i32(128)))
    # ctrl_num_c
    ifelse(eq(partition, k(PART_DATA, 4)), eq(var("ctrl_num"), var("ctrl_data_num")),
           eq(var("ctrl_num"), var("ctrl_info_num")))
    # rand_op_c
    for ro, part in ((OP_READONLY_ON_INFO_PARTITION, PART_INFO),
                     (OP_READONLY_ON_INFO1_PARTITION, PART_INFO1)):
        avoid = bit(AVOID_RO_PARTITIONS)
        cond(bit(ro), land(implies(b, avoid, b.expr_binary(BIN_NEQ, partition, k(part, 4))),
                           implies(b, neg(avoid),
                                   implies(b, eq(partition, k(part, 4)),
                                           eq(rop, k(FLASH_OP_READ, 2))))))
    b.add_constraint(b.expr_in_set(partition, [k(x, 4) for x in (PART_DATA, PART_INFO,
                                                                  PART_INFO1, PART_INFO2)]))
    b.add_constraint(eq(ext(addr, TL_AW - 1, BUS_ADDR_BYTE_W), u32(0)))
    b.add_constraint(eq(ext(addr, 1, 0), u32(0)))
    cond(bit(ADDR_FLASH_WORD_ALIGNED), eq(ext(addr, 2, 2), k(0, 1)))
    cond(eq(fractions, i32(16)), eq(ext(addr, 2, 2), i32(0)))
    hi = b.expr_binary(BIN_SUB, const_array(pidx, INFO_TYPE_BYTES, u32),
                       b.expr_binary(BIN_MUL, u32(FLASH_BANK_BYTES_PER_WORD), fractions))
    not_data = b.expr_binary(BIN_NEQ, partition, k(PART_DATA, 4))
    ifelse(not_data, land(b.expr_in_range(addr, i32(0), hi), eq(prog_sel, i32(1))),
           eq(prog_sel, i32(0)))
    is_data = eq(partition, k(PART_DATA, 4))
    cond(ecc_on, land(implies(b, is_data, eq(ext(addr, 18, 17), k(TGT_PRE_RD, 2))),
                      implies(b, neg(is_data), eq(ext(addr, 10, 9), k(TGT_PRE_RD, 2)))))
    b.add_constraint(eq(otf_addr, ext(addr, BUS_ADDR_BYTE_W - 2, 0)))
    b.add_constraint(eq(num_words, fractions))
    # special_info_acc_c
    acc = var("allow_spec_info_acc")
    b.add_constraint(b.expr_binary(BIN_OR, b.expr_in_set(acc, [k(7, 3), k(0, 3)]),
                                   b.expr_in_range(acc, i32(1), i32(6))))

