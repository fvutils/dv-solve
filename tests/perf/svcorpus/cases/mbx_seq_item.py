"""OpenTitan mbx: inbound/outbound mailbox address windows and message sizes.

    rand bit [top_pkg::TL_AW-1:0] ibmbx_base_addr, ibmbx_limit_addr,
                                  obmbx_base_addr, obmbx_limit_addr;   // 32 bits
    rand bit address_range_valid;
    rand mubi4_t address_range_regwen;
    rand bit [10:0] request_dwords, response_dwords;
    constraint legal_ibmbx_addr_range_c {
      (ibmbx_limit_addr < 32'h4000_0000);
      (ibmbx_limit_addr >= ibmbx_base_addr); }
    constraint imbx_addr_range_lock_limit_c {
      ((ibmbx_limit_addr - ibmbx_base_addr) / 4 >= MBX_DV_MAX_DW); }
    constraint legal_obmbx_addr_range_c {
      (obmbx_limit_addr < 32'h4000_0000);
      (obmbx_limit_addr >= obmbx_base_addr); }
    constraint legal_address_range_valid_c { address_range_valid == 1'b1; }
    constraint legal_request_dwords_c { request_dwords > 0 && request_dwords < 'h400; }
    constraint legal_response_dwords_c {
      response_dwords > 0 && response_dwords <= MBX_DV_MAX_DW; }
    constraint legal_non_overlapping_region_c {
      ibmbx_limit_addr < obmbx_base_addr || ibmbx_base_addr > obmbx_limit_addr; }
    constraint legal_addr_alignment_c {
      (ibmbx_base_addr[1:0] == 0); (ibmbx_limit_addr[1:0] == 0);
      (obmbx_base_addr[1:0] == 0); (obmbx_limit_addr[1:0] == 0); }

TL_AW = 32; MBX_DV_MAX_DW = 1024 (`parameter int unsigned`, mbx_env_pkg).
address_range_regwen (rand mubi4_t) is in no constraint: Verilator draws it
outside the solver (only its named values, 4'h6/4'h9) and it is not in the
query, so the case leaves it out too.
"""
from dv_solve.problem import BIN_AND, BIN_DIV, BIN_EQ, BIN_GT, BIN_GTE, BIN_LT, BIN_LTE, BIN_OR, BIN_SUB

NAME = "mbx_seq_item"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/ip/mbx/dv/env/mbx_seq_item.sv", "lines": "5-92"}
ABOUT = "two disjoint aligned address windows below 1 GiB, one at least 4 KiB, two message sizes"

VARS = [("ibmbx_base_addr", 32, False), ("ibmbx_limit_addr", 32, False),
        ("obmbx_base_addr", 32, False), ("obmbx_limit_addr", 32, False),
        ("address_range_valid", 1, False),
        ("request_dwords", 11, False), ("response_dwords", 11, False)]
DIST = {}
SOLVE_BEFORE = []

MBX_DV_MAX_DW = 1024


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    bin_ = b.expr_binary
    i32 = lambda x: b.expr_const(x, is_signed=True, width=32)   # noqa: E731  unsized literal
    u32 = lambda x: b.expr_const(x, width=32)              # noqa: E731  32'h / 'h / int unsigned
    ib, il = var("ibmbx_base_addr"), var("ibmbx_limit_addr")
    ob, ol = var("obmbx_base_addr"), var("obmbx_limit_addr")
    b.add_constraint(bin_(BIN_LT, il, u32(0x4000_0000)))
    b.add_constraint(bin_(BIN_GTE, il, ib))
    b.add_constraint(bin_(BIN_GTE, bin_(BIN_DIV, bin_(BIN_SUB, il, ib), i32(4)), u32(MBX_DV_MAX_DW)))
    b.add_constraint(bin_(BIN_LT, ol, u32(0x4000_0000)))
    b.add_constraint(bin_(BIN_GTE, ol, ob))
    b.add_constraint(bin_(BIN_EQ, var("address_range_valid"), b.expr_const(1, width=1)))
    rq, rs = var("request_dwords"), var("response_dwords")
    b.add_constraint(bin_(BIN_AND, bin_(BIN_GT, rq, i32(0)), bin_(BIN_LT, rq, u32(0x400))))
    b.add_constraint(bin_(BIN_AND, bin_(BIN_GT, rs, i32(0)), bin_(BIN_LTE, rs, u32(MBX_DV_MAX_DW))))
    b.add_constraint(bin_(BIN_OR, bin_(BIN_LT, il, ob), bin_(BIN_GT, ib, ol)))
    for a in (ib, il, ob, ol):
        b.add_constraint(bin_(BIN_EQ, b.expr_extract(a, 1, 0), i32(0)))
