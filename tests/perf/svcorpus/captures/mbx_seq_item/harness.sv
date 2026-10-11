// OpenTitan hw/ip/mbx/dv/env/mbx_seq_item.sv, all constraint blocks
package p;
  localparam int TL_AW = 32;
  parameter int unsigned MBX_DV_MAX_DW = 1024;
  typedef enum logic [3:0] {
    MuBi4True = 4'h6,
    MuBi4False = 4'h9
  } mubi4_t;
endpackage
import p::*;
class mbx_seq_item;
  rand bit [TL_AW-1:0] ibmbx_base_addr;
  rand bit [TL_AW-1:0] ibmbx_limit_addr;
  rand bit [TL_AW-1:0] obmbx_base_addr;
  rand bit [TL_AW-1:0] obmbx_limit_addr;
  rand bit address_range_valid;
  rand mubi4_t address_range_regwen;
  rand bit [10:0] request_dwords;
  rand bit [10:0] response_dwords;

  constraint legal_ibmbx_addr_range_c {
    (ibmbx_limit_addr < 32'h4000_0000);
    (ibmbx_limit_addr >= ibmbx_base_addr);
  }
  constraint imbx_addr_range_lock_limit_c {
    ((ibmbx_limit_addr - ibmbx_base_addr) / 4 >= MBX_DV_MAX_DW);
  }
  constraint legal_obmbx_addr_range_c {
    (obmbx_limit_addr < 32'h4000_0000);
    (obmbx_limit_addr >= obmbx_base_addr);
  }
  constraint legal_address_range_valid_c {
    address_range_valid == 1'b1;
  }
  constraint legal_request_dwords_c {
    request_dwords > 0 && request_dwords < 'h400;
  }
  constraint legal_response_dwords_c {
    response_dwords > 0 && response_dwords <= MBX_DV_MAX_DW;
  }
  constraint legal_non_overlapping_region_c {
    ibmbx_limit_addr < obmbx_base_addr || ibmbx_base_addr > obmbx_limit_addr;
  }
  constraint legal_addr_alignment_c {
    (ibmbx_base_addr[1:0] == 0);
    (ibmbx_limit_addr[1:0] == 0);
    (obmbx_base_addr[1:0] == 0);
    (obmbx_limit_addr[1:0] == 0);
  }
endclass
module top;
  initial begin
    mbx_seq_item t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d %h %h %h %h %b %h %0d %0d", i, t.ibmbx_base_addr, t.ibmbx_limit_addr,
               t.obmbx_base_addr, t.obmbx_limit_addr, t.address_range_valid,
               t.address_range_regwen, t.request_dwords, t.response_dwords);
    end
    $finish;
  end
endmodule
