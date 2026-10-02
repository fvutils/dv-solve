// Randomize a bus packet through DPI. The constraints come from
// packet_problem_pkg.sv, which packet_problem.py generates.
import dvs_dpi_pkg::*;
import dvs_randomizer_pkg::*;
import packet_problem_pkg::*;

class Packet;
  bit [31:0] addr;
  bit [7:0]  len;
  bit [3:0]  kind;
endclass

class PacketRandomizer extends dvs_randomizer #(Packet);
  virtual function string get_problem_b64();
    return PROBLEM;
  endfunction

  virtual function void apply_solution(Packet obj, chandle ctx);
    obj.addr = 32'(dvs_dpi_get_value_h(ctx, ADDR));
    obj.len  = 8'(dvs_dpi_get_value_h(ctx, LEN));
    obj.kind = 4'(dvs_dpi_get_value_h(ctx, KIND));
  endfunction
endclass

module top;
  initial begin
    automatic Packet p = new;
    automatic PacketRandomizer r = new;
    chandle h;
    int cp;

    // Randomize through the base class.
    repeat (5) begin
      if (r.randomize_obj(p, longint'($urandom())) != 0) $fatal(1, "randomize_obj failed");
      $display("addr=%h len=%0d kind=%0d", p.addr, p.len, p.kind);
    end
    r.cleanup();

    // The same problem through the handle API, with kind pinned to 7.
    h = dvs_dpi_compile_b64(PROBLEM);
    if (h == null) $fatal(1, "compile failed");
    cp = dvs_dpi_checkpoint_h(h);
    if (dvs_dpi_pin_var_h(h, KIND, 7) != 0) $fatal(1, "pin failed");
    repeat (3) begin
      if (dvs_dpi_solve_h(h, longint'($urandom())) != 0) $fatal(1, "solve failed");
      $display("pinned: len=%0d", dvs_dpi_get_value_h(h, LEN));
    end
    dvs_dpi_restore_h(h, cp);   // kind is free again
    dvs_dpi_release_h(h);
    $finish;
  end
endmodule
