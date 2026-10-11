// OpenTitan hw/dv/sv/jtag_agent/jtag_item.sv, all constraint blocks
package p;
  typedef bit [31:0] uint;
  parameter uint JTAG_IRW = 32; // max IR width
  parameter uint JTAG_DRW = 64; // max DR width
  typedef enum bit {
    BusOpWrite = 1'b0,
    BusOpRead  = 1'b1
  } bus_op_e;
endpackage
import p::*;
class jtag_item;
  rand uint ir_len;
  rand uint dr_len;
  rand logic [JTAG_IRW-1:0] ir;
  rand logic [JTAG_DRW-1:0] dr;
  rand logic [JTAG_DRW-1:0] dout;
  rand bus_op_e bus_op;
  rand bit skip_reselected_ir;
  rand uint ir_pause_count;
  rand uint dr_pause_count;
  rand uint dr_pause_cycle;
  rand uint ir_pause_cycle;
  rand bit dummy_ir;
  rand bit dummy_dr;
  rand bit exit_via_pause_ir;
  rand bit exit_via_pause_dr;
  rand bit exit_to_rti_dr;
  rand bit reset_tap_fsm;

  constraint ir_len_c {
    ir_len <= JTAG_IRW;
  }
  constraint dr_len_c {
    dr_len <= JTAG_DRW;
  }
  constraint exit_to_rti_dr_c {
    soft exit_to_rti_dr == 1;
  }
  constraint reset_tap_fsm_c {
    soft reset_tap_fsm == 0;
  }
  constraint ir_dr_non_zero_c {
    ir_len > 0 || dr_len > 0;
  }
  constraint skip_reselected_ir_c {
    skip_reselected_ir dist {0:/3, 1:/7};
  }
  constraint ir_pause_count_c {
    ir_pause_count dist {0:/80, [1:40]:/ 20};
  }
  constraint dr_pause_count_c {
    dr_pause_count dist {0:/ 80, [1:40]:/ 20};
  }
  constraint dr_pause_cycle_c {
    if (dr_pause_count > 0 && dr_len > 3) {
      dr_pause_cycle inside {[1:dr_len-3]};
    } else {
      dr_pause_cycle == 0;
    }
    solve dr_len before dr_pause_cycle;
    solve dr_pause_count before dr_pause_cycle;
  }
  constraint ir_pause_cycle_c {
    if (ir_pause_count > 0 && ir_len > 3) {
      ir_pause_cycle inside {[1:ir_len-3]};
    } else {
      ir_pause_cycle == 0;
    }
    solve ir_len before ir_pause_cycle;
    solve ir_pause_count before ir_pause_cycle;
  }
  constraint dummy_ir_c {
    dummy_ir dist { 0 := 99, 1 := 1};
  }
  constraint dummy_dr_c {
    soft dummy_dr == 0;
  }
  constraint exit_via_pause_ir_c {
    exit_via_pause_ir dist { 0 := 90, 1 := 10};
  }
  constraint exit_via_pause_dr_c {
    exit_via_pause_dr dist { 0 := 90, 1 := 10};
  }
endclass
module top;
  initial begin
    jtag_item t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d ir_len=%0d dr_len=%0d ipc=%0d dpc=%0d ipcy=%0d dpcy=%0d skip=%0d dir=%0d ddr=%0d epi=%0d epd=%0d rti=%0d rst=%0d op=%0d", i, t.ir_len, t.dr_len, t.ir_pause_count, t.dr_pause_count, t.ir_pause_cycle, t.dr_pause_cycle, t.skip_reselected_ir, t.dummy_ir, t.dummy_dr, t.exit_via_pause_ir, t.exit_via_pause_dr, t.exit_to_rti_dr, t.reset_tap_fsm, t.bus_op);
    end
    $finish;
  end
endmodule
