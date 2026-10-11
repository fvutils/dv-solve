// OpenTitan hw/dv/sv/tl_agent/tl_seq_item.sv, all constraint blocks
// Widths from tl_agent_pkg / bus_params_pkg / top_earlgrey top_pkg.
package p;
  typedef enum logic [2:0] {
    PutFullData    = 3'h 0,
    PutPartialData = 3'h 1,
    Get            = 3'h 4
  } tl_a_op_e;
  typedef enum logic [2:0] {
    AccessAck     = 3'h 0,
    AccessAckData = 3'h 1
  } tl_d_op_e;
  parameter int AddrWidth   = 32;
  parameter int DataWidth   = 32;
  parameter int MaskWidth   = 32 >> 3;
  parameter int SizeWidth   = $clog2($clog2(MaskWidth)+1);
  parameter int SourceWidth = 8;
  parameter int AUserWidth  = 23;
  parameter int DUserWidth  = 14;
  parameter int OpcodeWidth = 3;
endpackage
import p::*;

`define A_OPCODE_IS_VALID(a_opcode) (a_opcode inside {Get, PutFullData, PutPartialData})
`define MASK_IS_FULL(a_mask, a_size) ($countones(a_mask) == (1 << a_size))
`define MASK_LOW_IN_INACTIVE_LANES(a_mask, a_addr, a_size) \
    ((a_mask & ~(((1 << (1 << a_size)) - 1) << a_addr[SizeWidth-1:0])) == 0)
`define ADDR_ALIGNED_TO_SIZE(a_addr, a_size) ((a_addr & ((1 << a_size) - 1)) == 0)
`define BELOW_MAX_A_SIZE(a_size) (a_size <= 2)

class tl_seq_item;
  rand bit [AddrWidth - 1   : 0] a_addr;
  rand bit [DataWidth - 1   : 0] a_data;
  rand bit [MaskWidth - 1   : 0] a_mask;
  rand bit [SizeWidth - 1   : 0] a_size;
  rand bit [2:0]                 a_param;
  rand bit [SourceWidth - 1 : 0] a_source;
  rand bit [OpcodeWidth - 1 : 0] a_opcode;
  rand bit [AUserWidth - 1 : 0]  a_user;
  rand bit [2:0]                 d_param;
  rand bit [DataWidth - 1   : 0] d_data;
  rand bit [SourceWidth - 1 : 0] d_source;
  rand bit [SizeWidth - 1   : 0] d_size;
  rand bit [OpcodeWidth - 1 : 0] d_opcode;
  rand bit                       d_error;
  rand bit [DUserWidth - 1 : 0]  d_user;
  rand bit                       d_sink;
  rand int unsigned               a_valid_delay;
  rand int unsigned               a_valid_len;
  rand int unsigned               d_valid_delay;
  rand int unsigned               d_valid_len;

  constraint param_c {
    a_param == 0;
    d_param == 0;
  }
  constraint no_d_error_c {
    soft d_error == 0;
  }
  constraint a_valid_len_c {
    a_valid_len inside {[1:10]};
  }
  constraint d_valid_len_c {
    d_valid_len inside {[1:10]};
  }
  constraint valid_delay_c {
    a_valid_delay inside {[0:50]};
    d_valid_delay inside {[0:50]};
  }
  constraint d_opcode_c {
    d_opcode inside {AccessAckData, AccessAck};
  }
  constraint a_opcode_c {
    `A_OPCODE_IS_VALID(a_opcode);
  }
  constraint mask_contiguous_c {
    $countones(a_mask ^ {a_mask[MaskWidth-2:0], 1'b0}) <= 2;
  }
  constraint mask_w_PutFullData_c {
    a_opcode == PutFullData -> `MASK_IS_FULL(a_mask, a_size);
  }
  constraint mask_in_active_lanes_c {
    `MASK_LOW_IN_INACTIVE_LANES(a_mask, a_addr, a_size);
  }
  constraint addr_size_align_c {
    `ADDR_ALIGNED_TO_SIZE(a_addr, a_size);
  }
  constraint max_size_c {
    `BELOW_MAX_A_SIZE(a_size);
  }
endclass
module top;
  initial begin
    tl_seq_item t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d addr=%h mask=%b size=%0d op=%0d dop=%0d derr=%0d len=%0d/%0d dly=%0d/%0d", i, t.a_addr, t.a_mask, t.a_size, t.a_opcode, t.d_opcode, t.d_error, t.a_valid_len, t.d_valid_len, t.a_valid_delay, t.d_valid_delay);
    end
    $finish;
  end
endmodule
