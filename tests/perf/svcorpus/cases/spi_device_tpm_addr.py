"""OpenTitan spi_device: a TPM transaction's address and size.

    localparam bit [TPM_ADDR_WIDTH-1:0] TPM_ADDR_WORD_ALIGN_MASK = '1 << 2;
    rand bit is_hw_reg_offset, is_hw_reg_region, is_valid_locality,
             is_addr_word_aligned;
    rand bit [TPM_ADDR_WIDTH-1:0] tpm_addr;            // 24 bits
    constraint tpm_addr_c {
      solve is_hw_reg_offset, is_hw_reg_region, is_valid_locality,
            is_addr_word_aligned before tpm_addr;
      is_hw_reg_offset -> (tpm_addr[TPM_OFFSET_WIDTH-1:0] & TPM_ADDR_WORD_ALIGN_MASK)
                          inside {ALL_TPM_HW_REG_OFFSETS};
      is_hw_reg_region -> tpm_addr inside {[24'hD4_0000:24'hD4_FFFF]};
      is_valid_locality ->
        tpm_addr[TPM_OFFSET_WIDTH+TPM_LOCALITY_WIDTH-1:TPM_OFFSET_WIDTH] < MAX_TPM_LOCALITY;
      is_addr_word_aligned -> tpm_addr[1:0] == 0;
    }
    rand uint tpm_size;
    constraint tpm_size_c {
      tpm_size dist { [1:3] :/ 1, 4 :/ 2, [5:MAX_SUPPORT_TPM_SIZE-1] :/ 2,
                      MAX_SUPPORT_TPM_SIZE :/ 1 };
    }
    rand uint tpm_write;

TPM_OFFSET_WIDTH = 12, TPM_LOCALITY_WIDTH = 4, MAX_TPM_LOCALITY = 5 (all
`parameter byte`), MAX_SUPPORT_TPM_SIZE = 64; ALL_TPM_HW_REG_OFFSETS are
12-bit register offsets (TPM_INT_STATUS_OFFSET is listed twice).
"""
from dv_solve.problem import BIN_BAND, BIN_EQ, BIN_LT

from tests.perf.svcorpus.case import implies, inside_dist

NAME = "spi_device_tpm_addr"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/ip/spi_device/dv/env/seq_lib/spi_device_tpm_base_vseq.sv",
          "lines": "10-41"}
ABOUT = "address fields under four knob bits: a register-offset set, a region, a locality field, alignment"

VARS = [("is_hw_reg_offset", 1, False), ("is_hw_reg_region", 1, False),
        ("is_valid_locality", 1, False), ("is_addr_word_aligned", 1, False),
        ("tpm_addr", 24, False), ("tpm_size", 32, False), ("tpm_write", 32, False)]
DIST = {"tpm_size": [(1, 3, 1, False), (4, 4, 2, False), (5, 63, 2, False), (64, 64, 1, False)]}
SOLVE_BEFORE = [(("is_hw_reg_offset", "is_hw_reg_region", "is_valid_locality",
                  "is_addr_word_aligned"), ("tpm_addr",))]

HW_REG_OFFSETS = [0x0, 0x8, 0xC, 0x10, 0x10, 0x18, 0x28, 0xF00, 0xF04]


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    addr = var("tpm_addr")
    mask = b.expr_const(0xFFFFFC, width=24)                # '1 << 2, 24 bits
    off = b.expr_binary(BIN_BAND, b.expr_extract(addr, 11, 0), mask)
    b.add_constraint(implies(b, var("is_hw_reg_offset"),
                             b.expr_in_set(off, [b.expr_const(k, width=12) for k in HW_REG_OFFSETS])))
    b.add_constraint(implies(b, var("is_hw_reg_region"),
                             b.expr_in_range(addr, b.expr_const(0xD40000, width=24),
                                             b.expr_const(0xD4FFFF, width=24))))
    b.add_constraint(implies(b, var("is_valid_locality"),
                             b.expr_binary(BIN_LT, b.expr_extract(addr, 15, 12),
                                           b.expr_const(5, is_signed=True, width=8))))
    b.add_constraint(implies(b, var("is_addr_word_aligned"),
                             b.expr_binary(BIN_EQ, b.expr_extract(addr, 1, 0),
                                           b.expr_const(0, is_signed=True, width=32))))
    b.add_constraint(inside_dist(b, var("tpm_size"), DIST["tpm_size"]))
