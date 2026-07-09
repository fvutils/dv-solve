// Timing benchmark: many randomize() calls under an external solver.
// Iteration count is passed at runtime via +iters=<N> (default 2000).
// Keeps constraints identical to t_smoke_unsigned.sv but drops the per-call
// $display/diversity bookkeeping so we measure solver time, not TB overhead.
//
// SPDX-License-Identifier: CC0-1.0

class Packet;
  rand bit [7:0] a;
  rand bit [7:0] b;
  rand bit [3:0] sel;

  constraint c_order  { a < b; }
  constraint c_range  { a > 8'd10; b < 8'd240; }
  constraint c_sum    { (a + b) < 9'd300; }
  constraint c_sel    { sel inside {[4'd2:4'd9]}; }
  constraint c_imp    { (sel == 4'd5) -> (a == 8'd42); }
endclass

module t;
  Packet p = new;
  int    iters;
  int    fail;
  longint acc;   // accumulate values so the loop can't be optimized away
  integer i;

  initial begin
    if (!$value$plusargs("iters=%d", iters)) iters = 2000;
    fail = 0;
    acc = 0;

    for (i = 0; i < iters; i = i + 1) begin
      if (!p.randomize()) begin
        fail = fail + 1;
      end else begin
        // Light legality spot-check (cheap; keeps the model honest).
        if (!(p.a < p.b) || !(p.a > 10) || (p.sel == 5 && p.a != 42))
          fail = fail + 1;
        acc = acc + p.a + p.b + p.sel;
      end
    end

    $display("BENCH iters=%0d fail=%0d acc=%0d", iters, fail, acc);
    if (fail != 0) begin
      $display("FAIL: %0d illegal/failed solves", fail);
      $stop;
    end
    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule
