// Minimal: one small loosely-constrained field. Establishes the per-randomize floor.
class T; rand bit [7:0] x; constraint c { x inside {[1:200]}; } endclass
module t;
  T o = new; int i, fail;
  initial begin
    if (!$value$plusargs("iters=%d", i)) i = 20000;
    for (int n=0;n<i;n++) begin
      if (!o.randomize()) fail++;
      if (!(o.x>=1 && o.x<=200)) fail++;
    end
    $display("BENCH iters=%0d fail=%0d", i, fail);
    if (fail!=0) begin $display("FAIL"); $stop; end
    $write("*-* All Finished *-*\n"); $finish;
  end
endmodule
