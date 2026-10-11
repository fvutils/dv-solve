// OpenTitan hw/ip/hmac/dv/env/seq_lib/hmac_base_vseq.sv: rand fields + constraint blocks
// (UVM stripped; extern constraints written in the class body)
package p;
  typedef int unsigned uint;
  typedef bit [31:0] uint32;
  parameter int TL_AW  = 32;
  parameter int TL_DW  = 32;
  parameter int TL_DBW = (TL_DW >> 3);
  parameter uint32 HMAC_MSG_FIFO_SIZE      = 2048;
  parameter uint32 HMAC_MSG_FIFO_BASE      = 32'h1000;
  parameter uint32 HMAC_MSG_FIFO_LAST_ADDR = HMAC_MSG_FIFO_BASE + HMAC_MSG_FIFO_SIZE - 1;
  parameter uint NUM_KEYS = 32;
endpackage
import p::*;
class hmac_base_vseq;
  rand bit [TL_AW-1:0]    wr_addr;
  rand bit [TL_DBW-1:0]   wr_mask;
  rand bit                wr_config_during_hash;
  rand bit                wr_key_during_hash;
  rand bit                hmac_en;
  rand bit [3:0]          digest_size;
  rand bit [31:0]         key[NUM_KEYS];
  rand bit [5:0]          key_length;
  rand bit                endian_swap;
  rand bit                digest_swap;
  rand bit                key_swap;

  constraint wr_addr_c {
    wr_addr inside {[HMAC_MSG_FIFO_BASE : HMAC_MSG_FIFO_LAST_ADDR]};
  }
  constraint wr_mask_c {
    $countones(wr_mask) dist {
      TL_DBW       :/ 1,
      [1:TL_DBW-1] :/ 1
    };
  }
  constraint key_length_c {
    ($countones(key_length) == 1) dist {
      1 :/ 4,  // Key_128/Key_256/Key_384/Key_512/Key_1024/Key_None
      0 :/ 1   // Illegal -> should get casted to Key_None in HW
    };
  }
  constraint digest_size_c {
    ($countones(digest_size) == 1) dist {
      1 :/ 4,  // SHA2_256/SHA2_384/SHA2_512/SHA2_None
      0 :/ 1   // Illegal -> should get casted to SHA2_None in HW
    };
  }
  constraint wr_mask_contiguous_c {
    $countones(wr_mask ^ {wr_mask[TL_DBW-2:0], 1'b0}) <= 2; // mask must have contiguous ones
  }
endclass
module top;
  initial begin
    hmac_base_vseq s = new;
    for (int i = 0; i < 20; i++) begin
      if (!s.randomize()) $display("FAIL");
      $display("%0d %h %b %b%b%b %b %b %b%b%b %h", i, s.wr_addr, s.wr_mask, s.wr_config_during_hash,
               s.wr_key_during_hash, s.hmac_en, s.digest_size, s.key_length, s.endian_swap,
               s.digest_swap, s.key_swap, s.key[0]);
    end
    $finish;
  end
endmodule
