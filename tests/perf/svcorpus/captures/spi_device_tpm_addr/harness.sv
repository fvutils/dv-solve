// OpenTitan hw/ip/spi_device/dv/env/seq_lib/spi_device_tpm_base_vseq.sv, tpm_addr_c + tpm_size_c
package p;
  parameter byte MAX_TPM_LOCALITY = 5;
  parameter byte TPM_OFFSET_WIDTH = 12;
  parameter byte TPM_LOCALITY_WIDTH = 4;
  parameter int  TPM_ADDR_WIDTH = 24;
  parameter int  MAX_SUPPORT_TPM_SIZE = 64;
  parameter bit[11:0] ALL_TPM_HW_REG_OFFSETS[9] = '{12'h0, 12'h8, 12'hC, 12'h10, 12'h10, 12'h18, 12'h28, 12'hF00, 12'hF04};
endpackage
import p::*;
class tpm_item;
  localparam bit [TPM_ADDR_WIDTH-1:0] TPM_ADDR_WORD_ALIGN_MASK = '1 << 2;
  rand bit        is_hw_reg_offset;
  rand bit        is_hw_reg_region;
  rand bit        is_valid_locality;
  rand bit        is_addr_word_aligned;
  rand bit [TPM_ADDR_WIDTH-1:0] tpm_addr;
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
  rand int unsigned tpm_size;
  constraint tpm_size_c {
    tpm_size dist {
      [1:3]              :/ 1,
      4                  :/ 2,
      [5:MAX_SUPPORT_TPM_SIZE-1] :/ 2,
      MAX_SUPPORT_TPM_SIZE       :/ 1
    };
  }
  rand int unsigned tpm_write;
endclass
module top;
  initial begin
    tpm_item t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d %h %0d %b%b%b%b", i, t.tpm_addr, t.tpm_size, t.is_hw_reg_offset, t.is_hw_reg_region, t.is_valid_locality, t.is_addr_word_aligned);
    end
    $finish;
  end
endmodule
