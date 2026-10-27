// arith.add_ovf: 8-bit add that wraps
class sb_item;
  rand bit [7:0] a, b, s;
  constraint c_a { s == a + b; a > 8'd200; b > 8'd100; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.add_ovf"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.s == 8'(it.a+it.b))) e++;
    if (!(it.a > 200 && it.b > 100)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.a, it.b, it.s);
  endfunction
endclass
