// Heavily coupled: 8 fields in an ordering + sum chain (stresses coupled diversity).
class T;
  rand bit [7:0] a,b,c,d,e,f,g,h;
  constraint ord { a<b; b<c; c<d; d<e; e<f; f<g; g<h; }
  constraint sum { (a+b+c+d) < 9'd400; }
  constraint mod { (h % 4) == 0; }
endclass
module t;
  T o = new; int i, fail;
  initial begin
    if (!$value$plusargs("iters=%d", i)) i = 20000;
    for (int n=0;n<i;n++) begin
      if (!o.randomize()) fail++;
      if (!(o.a<o.b && o.g<o.h)) fail++;
    end
    $display("BENCH iters=%0d fail=%0d", i, fail);
    if (fail!=0) begin $display("FAIL"); $stop; end
    $write("*-* All Finished *-*\n"); $finish;
  end
endmodule
