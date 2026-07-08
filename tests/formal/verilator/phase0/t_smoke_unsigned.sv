// Phase-0 smoke test: dv-solve as a drop-in Verilator SMT solver.
//
// Deliberately UNSIGNED-only (no signed compares, no ~^, no replication) so it
// exercises the solver plumbing (pipe / prelude / check-sat / get-value / reset)
// and NOT the known operator gaps. Checks both legality and run-to-run diversity.
//
// SPDX-License-Identifier: CC0-1.0

class Packet;
  rand bit [7:0] a;
  rand bit [7:0] b;
  rand bit [3:0] sel;

  // Unsigned relational + arithmetic + range + implication constraints.
  constraint c_order  { a < b; }
  constraint c_range  { a > 8'd10; b < 8'd240; }
  constraint c_sum    { (a + b) < 9'd300; }
  constraint c_sel    { sel inside {[4'd2:4'd9]}; }
  constraint c_imp    { (sel == 4'd5) -> (a == 8'd42); }
endclass

module t;
  Packet p = new;
  int    ok_diversity;
  bit [7:0] prev_a;
  integer i;

  initial begin
    // First solve.
    if (!p.randomize()) begin
      $display("FAIL: first randomize() returned 0");
      $stop;
    end
    // Legality checks.
    if (!(p.a < p.b))                        begin $display("FAIL: a<b  a=%0d b=%0d", p.a, p.b); $stop; end
    if (!(p.a > 10))                         begin $display("FAIL: a>10 a=%0d", p.a); $stop; end
    if (!(p.b < 240))                        begin $display("FAIL: b<240 b=%0d", p.b); $stop; end
    if (!((p.a + p.b) < 300))                begin $display("FAIL: a+b<300 a=%0d b=%0d", p.a, p.b); $stop; end
    if (!(p.sel >= 2 && p.sel <= 9))         begin $display("FAIL: sel range sel=%0d", p.sel); $stop; end
    if (p.sel == 5 && p.a != 42)             begin $display("FAIL: implication sel=5 a=%0d", p.a); $stop; end
    prev_a = p.a;

    // Randomize a bunch more times; verify legality every time and that at
    // least one value differs (diversity).
    for (i = 0; i < 20; i = i + 1) begin
      if (!p.randomize()) begin
        $display("FAIL: randomize() returned 0 on iter %0d", i);
        $stop;
      end
      if (!(p.a < p.b))                begin $display("FAIL: a<b  iter %0d a=%0d b=%0d", i, p.a, p.b); $stop; end
      if (!(p.a > 10))                 begin $display("FAIL: a>10 iter %0d a=%0d", i, p.a); $stop; end
      if (!(p.b < 240))                begin $display("FAIL: b<240 iter %0d b=%0d", i, p.b); $stop; end
      if (!((p.a + p.b) < 300))        begin $display("FAIL: a+b<300 iter %0d", i); $stop; end
      if (!(p.sel >= 2 && p.sel <= 9)) begin $display("FAIL: sel range iter %0d sel=%0d", i, p.sel); $stop; end
      if (p.sel == 5 && p.a != 42)     begin $display("FAIL: implication iter %0d", i); $stop; end
      if (p.a != prev_a) ok_diversity = 1;
      prev_a = p.a;
    end

    if (ok_diversity != 1) begin
      $display("WARN: no diversity observed in 'a' across 21 solves (legal but repetitive)");
      // Not a hard fail for Phase-0: legality is the gate, diversity is a quality metric.
    end

    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule
