// OpenTitan hw/dv/sv/jtag_riscv_agent/seq_lib/jtag_riscv_csr_seq.sv, op_c
package p;
  typedef int unsigned uint;
  parameter uint DMI_OPW = 2;
  parameter uint DMI_DATAW = 32;
  typedef enum logic [DMI_OPW-1:0] {
    DmiStatus = 'h0,
    DmiRead   = 'h1,
    DmiWrite  = 'h2
  } jtag_op_e;
endpackage
import p::*;
class jtag_riscv_csr_seq;
  rand bit [  DMI_OPW-1:0] op;
  rand bit [DMI_DATAW-1:0] data;
  rand bit [DMI_DATAW+1:0] addr;
  rand bit                 do_write;
  constraint op_c {
    do_write == 1 -> op == DmiWrite;
    do_write == 0 -> op == DmiRead;
  }
endclass
module top;
  initial begin
    jtag_riscv_csr_seq t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d %0d %h %h %b", i, t.op, t.data, t.addr, t.do_write);
    end
    $finish;
  end
endmodule
