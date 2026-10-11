"""OpenTitan dma: a DMA transfer configuration (addresses, sizes, alignment).

    rand bit handshake, src_chunk_wrap, dst_chunk_wrap, src_addr_inc, dst_addr_inc,
             mem_range_valid;
    rand bit [63:0] src_addr, dst_addr;
    rand bit [31:0] mem_range_base, mem_range_limit, total_data_size, chunk_data_size,
                    soc_system_hi_addr;
    rand mubi4_t range_regwen;  rand opcode_e opcode;
    rand dma_transfer_width_e per_transfer_width;
    rand asid_encoding_e src_asid, dst_asid;
    rand bit [NumIntClearSources-1:0] clear_intr_src, clear_intr_bus, lsio_trigger_i;
    rand lsio_trigger_t handshake_intr_en;
    constraint lsio_trigger_i_c { (lsio_trigger_i & handshake_intr_en) != 0; }
    constraint transfer_width_c { if (valid_dma_config) {
      opcode inside {OpcSha256, OpcSha384, OpcSha512} -> per_transfer_width == DmaXfer4BperTxn; } }
    constraint src_config_c { src_addr_inc | src_chunk_wrap; }
    constraint dst_config_c { dst_addr_inc | dst_chunk_wrap; }
    constraint src_addr_c {
      if (valid_dma_config) {
        per_transfer_width == DmaXfer4BperTxn -> src_addr[1:0] == 2'd0;
        per_transfer_width == DmaXfer2BperTxn -> src_addr[0] == 1'b0;
        src_asid != SocSystemAddr -> src_addr[63:32] == '0;
        if (mem_range_valid && src_asid == OtInternalAddr && dst_asid != OtInternalAddr) {
          if (src_addr_in_range) {
            src_addr >= mem_range_base; src_addr <= mem_range_limit;
            mem_range_limit - src_addr >= chunk_data_size;
            if (!src_chunk_wrap) mem_range_limit - src_addr >= total_data_size;
          } else { ... }                       // src_addr_in_range is 1
        } }
      if (dma_dv_waive_system_bus && src_asid == SocSystemAddr) {
        src_addr[63:32] == soc_system_hi_addr; src_addr[31:0] <= ~total_data_size; } }
    constraint dst_addr_c { ... the same for dst, plus, under dst_addr_in_range:
          if (src_asid == OtInternalAddr)
            (dst_addr > src_addr + total_data_size + 'h10) ||
            (src_addr > dst_addr + total_data_size + 'h10); ... }
    constraint total_data_short_c { if (valid_dma_config) total_data_size inside {[1:1024]};
                                    else total_data_size inside {[1:'h10_0000]}; }
    constraint total_data_size_c { if (valid_dma_config) {
      total_data_size <= mem_range_limit - mem_range_base; total_data_size > 0; } }
    constraint chunk_data_size_c {
      if (valid_dma_config) { chunk_data_size <= mem_range_limit - mem_range_base;
                              chunk_data_size > 0; }
      if (handshake) { soft chunk_data_size inside {[1:256]}; }
      if (per_transfer_width == DmaXfer4BperTxn || !src_addr_inc || !dst_addr_inc)
        chunk_data_size[1:0] == '0;
      else per_transfer_width == DmaXfer2BperTxn -> chunk_data_size[0] == 1'b0;
      if (chunk_data_size < total_data_size) {
        opcode inside {OpcSha256, OpcSha384, OpcSha512} -> chunk_data_size[1:0] == 2'b00;
        (dst_addr_inc && !dst_chunk_wrap) -> chunk_data_size[1:0] == 2'b00;
        (src_addr_inc && !src_chunk_wrap) -> chunk_data_size[1:0] == 2'b00; }
      if (chunk_data_size < total_data_size) {
        per_transfer_width == DmaXfer4BperTxn -> chunk_data_size[1:0] == 2'd0;
        per_transfer_width == DmaXfer2BperTxn -> chunk_data_size[0] == 1'b0; } }
    constraint multi_chunk_c { chunk_data_size < total_data_size; }
    constraint mem_range_valid_c { if (valid_dma_config) mem_range_valid == 1'b1; }
    constraint mem_range_limit_c { if (valid_dma_config && mem_range_valid)
                                     mem_range_limit >= mem_range_base; }
    constraint range_regwen_c { if (!valid_dma_config) range_regwen == MuBi4True; }
    constraint handshake_intr_en_c { if (valid_dma_config) if (handshake)
                                       handshake_intr_en != 0; }

Non-rand state as constants: valid_dma_config = 1 (the default is 0; every
smoke vseq sets 1), src_addr_in_range = dst_addr_in_range = 1 (new()),
dma_dv_waive_system_bus = 1 (every test in dma_sim_cfg.hjson passes
+dma_dv_waive_system_bus=1). multi_chunk_c is commented "normally disabled"
but no sequence disables it, so it stays on. NumIntClearSources = 11.
The rand dynamic arrays intr_src_addr, intr_src_wr_val, sha2_digest are
only size-constrained (no element constraints): dropped from the harness and
the case. The soft constraint is in the case as add_soft_constraint;
Verilator's first query asserts it as a hard one (it drops softs only on
unsat), which equiv.py matches with soft_as_hard.
"""
from dv_solve.problem import (BIN_ADD, BIN_BAND, BIN_BOR, BIN_EQ, BIN_GT, BIN_GTE, BIN_LT,
                              BIN_LTE, BIN_NEQ, BIN_OR, BIN_SUB, UN_INVERT, UN_NOT)

from tests.perf.svcorpus.case import all_of, implies

NAME = "dma_seq_item"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/ip/dma/dv/env/dma_seq_item.sv", "lines": "34-378"}
ABOUT = "DMA transfer: 64-bit src/dst addresses in a memory window, sizes, alignment by width"

VARS = [("handshake", 1, False), ("src_addr", 64, False), ("dst_addr", 64, False),
        ("src_chunk_wrap", 1, False), ("dst_chunk_wrap", 1, False),
        ("src_addr_inc", 1, False), ("dst_addr_inc", 1, False), ("mem_range_valid", 1, False),
        ("mem_range_base", 32, False), ("mem_range_limit", 32, False),
        ("total_data_size", 32, False), ("chunk_data_size", 32, False),
        ("range_regwen", 4, False), ("opcode", 4, False), ("per_transfer_width", 2, False),
        ("src_asid", 4, False), ("dst_asid", 4, False),
        ("clear_intr_src", 11, False), ("clear_intr_bus", 11, False),
        ("handshake_intr_en", 11, False), ("soc_system_hi_addr", 32, False),
        ("lsio_trigger_i", 11, False)]
DIST = {}
SOLVE_BEFORE = [(("handshake_intr_en",), ("lsio_trigger_i",)),
                (("mem_range_base", "mem_range_limit"), ("src_addr",)),
                (("src_addr",), ("dst_addr",)),
                (("mem_range_limit",), ("total_data_size",)),
                (("mem_range_limit",), ("chunk_data_size",)),
                (("mem_range_base",), ("mem_range_limit",)),
                (("handshake",), ("handshake_intr_en",))]

VALID_DMA_CONFIG = 1
SRC_ADDR_IN_RANGE = 1
DST_ADDR_IN_RANGE = 1
DMA_DV_WAIVE_SYSTEM_BUS = 1

# enum encodings
MUBI4 = (0x6, 0x9)                                         # MuBi4True, MuBi4False
OPC_COPY, OPC_SHA256, OPC_SHA384, OPC_SHA512 = 0, 1, 2, 3
XFER1, XFER2, XFER4 = 0, 1, 2
OT_INTERNAL, SOC_CONTROL, SOC_SYSTEM = 0x7, 0xA, 0x9


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    k = lambda x, w: b.expr_const(x, width=w)    # noqa: E731  sized literal / enum item
    i32 = lambda x: b.expr_const(x, is_signed=True, width=32)   # unsized literal
    bit = lambda x: b.expr_const(x, width=1)
    eq = lambda a, c: b.expr_binary(BIN_EQ, a, c)          # noqa: E731
    neg = lambda a: b.expr_unary(UN_NOT, a)                # noqa: E731
    land = lambda *xs: all_of(b, *xs)                      # noqa: E731
    lor = lambda a, c: b.expr_binary(BIN_OR, a, c)         # noqa: E731
    cond = lambda c, *xs: b.add_constraint(implies(b, c, land(*xs)))   # noqa: E731
    sub = lambda a, c: b.expr_binary(BIN_SUB, a, c)       # noqa: E731

    # rand enums take only their named values
    b.add_constraint(b.expr_in_set(var("range_regwen"), [k(x, 4) for x in MUBI4]))
    b.add_constraint(b.expr_in_set(var("opcode"), [k(x, 4) for x in (0, 1, 2, 3)]))
    b.add_constraint(b.expr_in_set(var("per_transfer_width"), [k(x, 2) for x in (0, 1, 2)]))
    for a in ("src_asid", "dst_asid"):
        b.add_constraint(b.expr_in_set(var(a), [k(x, 4) for x in (OT_INTERNAL, SOC_CONTROL,
                                                                   SOC_SYSTEM)]))

    valid = bit(VALID_DMA_CONFIG)
    waive = bit(DMA_DV_WAIVE_SYSTEM_BUS)
    ptw = var("per_transfer_width")
    sha = lambda: b.expr_in_set(var("opcode"), [k(x, 4) for x in (OPC_SHA256, OPC_SHA384,
                                                                   OPC_SHA512)])
    base, limit = var("mem_range_base"), var("mem_range_limit")
    total, chunk = var("total_data_size"), var("chunk_data_size")

    # lsio_trigger_i_c
    b.add_constraint(b.expr_binary(BIN_NEQ, b.expr_binary(BIN_BAND, var("lsio_trigger_i"),
                                                          var("handshake_intr_en")), i32(0)))
    # transfer_width_c
    cond(valid, implies(b, sha(), eq(ptw, k(XFER4, 2))))
    # src_config_c, dst_config_c
    b.add_constraint(b.expr_binary(BIN_BOR, var("src_addr_inc"), var("src_chunk_wrap")))
    b.add_constraint(b.expr_binary(BIN_BOR, var("dst_addr_inc"), var("dst_chunk_wrap")))

    # src_addr_c / dst_addr_c
    for me, other, in_range in (("src", "dst", SRC_ADDR_IN_RANGE),
                                ("dst", "src", DST_ADDR_IN_RANGE)):
        addr = var(f"{me}_addr")
        asid, oasid = var(f"{me}_asid"), var(f"{other}_asid")
        wrap = var(f"{me}_chunk_wrap")
        left = lambda: sub(limit, addr)                    # noqa: E731
        if in_range:
            window = [b.expr_binary(BIN_GTE, addr, base), b.expr_binary(BIN_LTE, addr, limit),
                      b.expr_binary(BIN_GTE, left(), chunk),
                      implies(b, neg(wrap), b.expr_binary(BIN_GTE, left(), total))]
            if me == "dst":
                src, dst = var("src_addr"), var("dst_addr")
                gap = lambda a, c: b.expr_binary(                         # noqa: E731
                    BIN_GT, a, b.expr_binary(BIN_ADD, b.expr_binary(BIN_ADD, c, total),
                                             k(0x10, 32)))
                window.append(implies(b, eq(var("src_asid"), k(OT_INTERNAL, 4)),
                                      lor(gap(dst, src), gap(src, dst))))
            inner = land(*window)
        else:
            out = lambda size: lor(lor(b.expr_binary(BIN_LT, addr, base),   # noqa: E731
                                       b.expr_binary(BIN_GT, addr, limit)),
                                   b.expr_binary(BIN_LT, left(), size))
            inner = b.expr_ite(neg(wrap), out(total), out(chunk))
        cond(valid,
             implies(b, eq(ptw, k(XFER4, 2)), eq(b.expr_extract(addr, 1, 0), k(0, 2))),
             implies(b, eq(ptw, k(XFER2, 2)), eq(b.expr_extract(addr, 0, 0), k(0, 1))),
             implies(b, b.expr_binary(BIN_NEQ, asid, k(SOC_SYSTEM, 4)),
                     eq(b.expr_extract(addr, 63, 32), k(0, 32))),
             implies(b, land(var("mem_range_valid"), eq(asid, k(OT_INTERNAL, 4)),
                             b.expr_binary(BIN_NEQ, oasid, k(OT_INTERNAL, 4))),
                     inner))
        cond(land(waive, eq(asid, k(SOC_SYSTEM, 4))),
             eq(b.expr_extract(addr, 63, 32), var("soc_system_hi_addr")),
             b.expr_binary(BIN_LTE, b.expr_extract(addr, 31, 0),
                           b.expr_unary(UN_INVERT, total)))

    # total_data_short_c
    b.add_constraint(b.expr_ite(valid, b.expr_in_range(total, i32(1), i32(1024)),
                                b.expr_in_range(total, i32(1), k(0x10_0000, 32))))
    # total_data_size_c
    cond(valid, b.expr_binary(BIN_LTE, total, sub(limit, base)),
         b.expr_binary(BIN_GT, total, i32(0)))
    # chunk_data_size_c
    cond(valid, b.expr_binary(BIN_LTE, chunk, sub(limit, base)),
         b.expr_binary(BIN_GT, chunk, i32(0)))
    b.add_soft_constraint(implies(b, var("handshake"), b.expr_in_range(chunk, i32(1), i32(256))))
    c10 = lambda: b.expr_extract(chunk, 1, 0)               # noqa: E731
    b.add_constraint(b.expr_ite(
        lor(lor(eq(ptw, k(XFER4, 2)), neg(var("src_addr_inc"))), neg(var("dst_addr_inc"))),
        eq(c10(), k(0, 2)),                                 # '0
        implies(b, eq(ptw, k(XFER2, 2)), eq(b.expr_extract(chunk, 0, 0), k(0, 1)))))
    multi = lambda: b.expr_binary(BIN_LT, chunk, total)     # noqa: E731
    cond(multi(),
         implies(b, sha(), eq(c10(), k(0, 2))),
         implies(b, land(var("dst_addr_inc"), neg(var("dst_chunk_wrap"))), eq(c10(), k(0, 2))),
         implies(b, land(var("src_addr_inc"), neg(var("src_chunk_wrap"))), eq(c10(), k(0, 2))))
    cond(multi(),
         implies(b, eq(ptw, k(XFER4, 2)), eq(c10(), k(0, 2))),
         implies(b, eq(ptw, k(XFER2, 2)), eq(b.expr_extract(chunk, 0, 0), k(0, 1))))
    # multi_chunk_c
    b.add_constraint(multi())
    # mem_range_valid_c
    cond(valid, eq(var("mem_range_valid"), k(1, 1)))
    # mem_range_limit_c
    cond(land(valid, var("mem_range_valid")), b.expr_binary(BIN_GTE, limit, base))
    # range_regwen_c
    cond(neg(valid), eq(var("range_regwen"), k(MUBI4[0], 4)))
    # handshake_intr_en_c
    cond(valid, implies(b, var("handshake"),
                        b.expr_binary(BIN_NEQ, var("handshake_intr_en"), i32(0))))
