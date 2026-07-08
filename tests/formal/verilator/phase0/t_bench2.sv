// Richer randomization benchmark: multiple rand fields (signed + unsigned),
// relational / arithmetic / inside / implication constraints. Closer to a real
// DV constraint block than t_bench.sv. +iters=<N> (default 2000).
//
// SPDX-License-Identifier: CC0-1.0

class Txn;
  rand bit  [7:0]  len;
  rand bit  [15:0] addr;
  rand int         offset;      // signed
  rand bit  [3:0]  kind;
  rand bit  [7:0]  a, b, c;

  constraint c_len    { len inside {[1:64]}; }
  constraint c_addr   { addr < 16'hF000; (addr % 4) == 0; }
  constraint c_off    { offset > -100; offset < 100; }         // signed range
  constraint c_kind   { kind inside {[2:9]}; }
  constraint c_order  { a < b; b < c; }
  constraint c_sum    { (a + b + c) < 9'd400; }
  constraint c_imp    { (kind == 4'd5) -> (len > 8'd32); }
  constraint c_link   { (len > 8'd40) -> (addr > 16'h1000); }
endclass

module t;
  Txn tx = new;
  int i, fail;
  longint acc;
  initial begin
    if (!$value$plusargs("iters=%d", i)) i = 2000;
    fail = 0; acc = 0;
    for (int n = 0; n < i; n++) begin
      if (!tx.randomize()) begin fail++; continue; end
      // light legality spot-checks
      if (!(tx.len >= 1 && tx.len <= 64))       fail++;
      if (!(tx.addr < 16'hF000))                fail++;
      if (!(tx.offset > -100 && tx.offset < 100)) fail++;
      if (!(tx.a < tx.b && tx.b < tx.c))        fail++;
      if (tx.kind == 5 && !(tx.len > 32))       fail++;
      acc = acc + tx.len + tx.addr + tx.offset + tx.a + tx.b + tx.c;
    end
    $display("BENCH2 iters=%0d fail=%0d acc=%0d", i, fail, acc);
    if (fail != 0) begin $display("FAIL: %0d", fail); $stop; end
    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule
