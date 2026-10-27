// scenario.reset_reuse: small sum problem re-solved on one object (D test_reset_reuse)
class sb_item;
  rand bit [7:0] x;
  rand bit [7:0] y;
  rand bit [7:0] s;
  function bit ok();
    if (!(x <= 8'd99)) return 0;
    if (!(y <= 8'd99)) return 0;
    if (!(s == x + y)) return 0;
    if (!(s <= 8'd50)) return 0;
    return 1;
  endfunction
  constraint c_dom { x <= 8'd99; y <= 8'd99; }
  constraint c_sum { s == x + y; }
  constraint c_le { s <= 8'd50; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.reset_reuse"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.x, it.y, it.s);
  endfunction
endclass
