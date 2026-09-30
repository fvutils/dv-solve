// Randomize a bus packet with Verilator, using dv-solve as the constraint
// solver. Run with VERILATOR_SOLVER set; see the Verilator guide.
class Packet;
  rand bit [31:0] addr;
  rand bit [7:0]  len;
  rand bit [3:0]  kind;

  constraint c_align { addr[1:0] == 2'b00; }             // word aligned
  constraint c_len   { len inside {[1:16]}; }
  constraint c_fit   { addr + len < 32'h0000_1000; }     // inside a 4 KB window
  constraint c_kind  { kind != 0; (kind == 4'd7) -> (len > 8); }
endclass

module top;
  initial begin
    Packet p = new;
    repeat (5) begin
      if (!p.randomize()) $fatal(1, "randomize() failed");
      $display("addr=%h len=%0d kind=%0d", p.addr, p.len, p.kind);
    end
    $finish;
  end
endmodule
