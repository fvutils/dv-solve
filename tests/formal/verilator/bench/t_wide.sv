// Wide arithmetic: 32/64-bit fields with adds/relational constraints.
class T;
  rand bit [31:0] a, b;
  rand bit [63:0] hi;
  constraint c1 { a < b; }
  constraint c2 { (a + b) < 32'h8000_0000; }
  constraint c3 { hi > 64'd1000; hi < 64'd1000000; }
  constraint c4 { (a ^ b) != 0; }
endclass
module t;
  T o = new; int i, fail;
  initial begin
    if (!$value$plusargs("iters=%d", i)) i = 20000;
    for (int n=0;n<i;n++) begin
      if (!o.randomize()) fail++;
      if (!(o.a < o.b)) fail++;
    end
    $display("BENCH iters=%0d fail=%0d", i, fail);
    if (fail!=0) begin $display("FAIL"); $stop; end
    $write("*-* All Finished *-*\n"); $finish;
  end
endmodule
