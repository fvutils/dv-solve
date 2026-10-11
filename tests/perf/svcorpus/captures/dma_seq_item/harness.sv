// OpenTitan hw/ip/dma/dv/env/dma_seq_item.sv, all constraint blocks
package p;
  parameter int NumIntClearSources = 11;
  typedef logic [NumIntClearSources-1:0] lsio_trigger_t;
  typedef enum logic [1:0] {
    DmaXfer1BperTxn = 2'h0,
    DmaXfer2BperTxn = 2'h1,
    DmaXfer4BperTxn = 2'h2
  } dma_transfer_width_e;
  parameter int unsigned ASID_WIDTH = 4;
  typedef enum logic [ASID_WIDTH-1:0] {
    OtInternalAddr = 4'h7,
    SocControlAddr = 4'ha,
    SocSystemAddr  = 4'h9
  } asid_encoding_e;
  typedef enum logic [3:0] {
    OpcCopy   = 4'h0,
    OpcSha256 = 4'h1,
    OpcSha384 = 4'h2,
    OpcSha512 = 4'h3
  } opcode_e;
  typedef enum logic [3:0] {
    MuBi4True = 4'h6,
    MuBi4False = 4'h9
  } mubi4_t;
endpackage
import p::*;
class dma_seq_item;
  rand bit handshake;
  rand bit [63:0] src_addr;
  rand bit [63:0] dst_addr;
  rand bit        src_chunk_wrap;
  rand bit        dst_chunk_wrap;
  rand bit        src_addr_inc;
  rand bit        dst_addr_inc;
  rand bit        mem_range_valid;
  rand bit [31:0] mem_range_base;
  rand bit [31:0] mem_range_limit;
  rand bit [31:0] total_data_size;
  rand bit [31:0] chunk_data_size;
  rand mubi4_t range_regwen;
  rand opcode_e opcode;
  rand dma_transfer_width_e per_transfer_width;
  rand asid_encoding_e src_asid;
  rand asid_encoding_e dst_asid;
  rand bit [NumIntClearSources-1:0] clear_intr_src;
  rand bit [NumIntClearSources-1:0] clear_intr_bus;
  rand lsio_trigger_t handshake_intr_en;
  // constants (non-rand state)
  bit valid_dma_config = 1;
  bit src_addr_in_range = 1;
  bit dst_addr_in_range = 1;
  bit dma_dv_waive_system_bus = 1;
  rand bit [31:0] soc_system_hi_addr;
  rand bit [NumIntClearSources - 1:0] lsio_trigger_i;

  constraint lsio_trigger_i_c {
    solve handshake_intr_en before lsio_trigger_i;
    (lsio_trigger_i & handshake_intr_en) != 0;
  }
  constraint transfer_width_c {
    if (valid_dma_config) {
      opcode inside {OpcSha256, OpcSha384, OpcSha512} -> per_transfer_width == DmaXfer4BperTxn;
    }
  }
  constraint src_config_c {
    src_addr_inc | src_chunk_wrap;
  }
  constraint dst_config_c {
    dst_addr_inc | dst_chunk_wrap;
  }
  constraint src_addr_c {
    solve mem_range_base, mem_range_limit before src_addr;
    if (valid_dma_config) {
      per_transfer_width == DmaXfer4BperTxn -> src_addr[1:0] == 2'd0;
      per_transfer_width == DmaXfer2BperTxn -> src_addr[0] == 1'b0;
      src_asid != SocSystemAddr -> src_addr[63:32] == '0;
      if (mem_range_valid && src_asid == OtInternalAddr && dst_asid != OtInternalAddr) {
        if (src_addr_in_range) {
          src_addr >= mem_range_base;
          src_addr <= mem_range_limit;
          mem_range_limit - src_addr >= chunk_data_size;
          if (!src_chunk_wrap) {
            mem_range_limit - src_addr >= total_data_size;
          }
        } else {
          if (!src_chunk_wrap) {
            src_addr < mem_range_base  ||
            src_addr > mem_range_limit ||
            mem_range_limit - src_addr < total_data_size;
          } else {
            src_addr < mem_range_base  ||
            src_addr > mem_range_limit ||
            mem_range_limit - src_addr < chunk_data_size;
          }
        }
      }
    }
    if (dma_dv_waive_system_bus && src_asid == SocSystemAddr) {
      src_addr[63:32] == soc_system_hi_addr;
      src_addr[31:0] <= ~total_data_size;
    }
  }
  constraint dst_addr_c {
    solve src_addr before dst_addr;
    if (valid_dma_config) {
      per_transfer_width == DmaXfer4BperTxn -> dst_addr[1:0] == 2'd0;
      per_transfer_width == DmaXfer2BperTxn -> dst_addr[0] == 1'b0;
      dst_asid != SocSystemAddr -> dst_addr[63:32] == '0;
      if (mem_range_valid && dst_asid == OtInternalAddr && src_asid != OtInternalAddr) {
        if (dst_addr_in_range) {
          dst_addr >= mem_range_base;
          dst_addr <= mem_range_limit;
          mem_range_limit - dst_addr >= chunk_data_size;
          if (!dst_chunk_wrap) {
            mem_range_limit - dst_addr >= total_data_size;
          }
          if (src_asid == OtInternalAddr) {
            (dst_addr > src_addr + total_data_size + 'h10) ||
            (src_addr > dst_addr + total_data_size + 'h10);
          }
        } else {
          if (!dst_chunk_wrap) {
            dst_addr < mem_range_base  ||
            dst_addr > mem_range_limit ||
            mem_range_limit - dst_addr < total_data_size;
          } else {
            dst_addr < mem_range_base  ||
            dst_addr > mem_range_limit ||
            mem_range_limit - dst_addr < chunk_data_size;
          }
        }
      }
    }
    if (dma_dv_waive_system_bus && dst_asid == SocSystemAddr) {
      dst_addr[63:32] == soc_system_hi_addr;
      dst_addr[31:0] <= ~total_data_size;
    }
  }
  constraint total_data_short_c {
    solve mem_range_limit before total_data_size;
    if (valid_dma_config) {
      total_data_size inside {[1:1024]};
    } else {
      total_data_size inside {[1:'h10_0000]};
    }
  }
  constraint total_data_size_c {
    solve mem_range_limit before total_data_size;
    if (valid_dma_config) {
      total_data_size <= mem_range_limit - mem_range_base;
      total_data_size > 0;
    }
  }
  constraint chunk_data_size_c {
    solve mem_range_limit before chunk_data_size;
    if (valid_dma_config) {
      chunk_data_size <= mem_range_limit - mem_range_base;
      chunk_data_size > 0;
    }
    if (handshake) {
      soft chunk_data_size inside {[1:256]};
    }
    if (per_transfer_width == DmaXfer4BperTxn || !src_addr_inc || !dst_addr_inc) {
      chunk_data_size[1:0] == '0;
    } else {
      per_transfer_width == DmaXfer2BperTxn -> chunk_data_size[0] == 1'b0;
    }
    if (chunk_data_size < total_data_size) {
      opcode inside {OpcSha256, OpcSha384, OpcSha512} -> chunk_data_size[1:0] == 2'b00;
      (dst_addr_inc && !dst_chunk_wrap) -> chunk_data_size[1:0] == 2'b00;
      (src_addr_inc && !src_chunk_wrap) -> chunk_data_size[1:0] == 2'b00;
    }
    if (chunk_data_size < total_data_size) {
      per_transfer_width == DmaXfer4BperTxn -> chunk_data_size[1:0] == 2'd0;
      per_transfer_width == DmaXfer2BperTxn -> chunk_data_size[0] == 1'b0;
    }
  }
  constraint multi_chunk_c {
    chunk_data_size < total_data_size;
  }
  constraint mem_range_valid_c {
    if (valid_dma_config) {
      mem_range_valid == 1'b1;
    }
  }
  constraint mem_range_limit_c {
    solve mem_range_base before mem_range_limit;
    if (valid_dma_config && mem_range_valid) {
      mem_range_limit >= mem_range_base;
    }
  }
  constraint range_regwen_c {
    if (!valid_dma_config) {
      range_regwen == MuBi4True;
    }
  }
  constraint handshake_intr_en_c {
    solve handshake before handshake_intr_en;
    if (valid_dma_config) {
      if (handshake) {
        handshake_intr_en != 0;
      }
    }
  }
endclass
module top;
  initial begin
    dma_seq_item t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d %h %h %h %h %h %h %h %h %h %b", i, t.src_addr, t.dst_addr, t.mem_range_base,
               t.mem_range_limit, t.total_data_size, t.chunk_data_size, t.src_asid, t.dst_asid,
               t.per_transfer_width, t.handshake);
    end
    $finish;
  end
endmodule
