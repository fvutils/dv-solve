// arith.wide_add_64: 64-bit add with carry out
class sb_item;
  rand bit [63:0] a, b, s;
  constraint c_a { s == a + b; a > 64'hF000_0000_0000_0000; b < 64'h1000_0000_0000_0000; b > 64'd1000; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.wide_add_64"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.s == it.a+it.b)) e++;
    if (!(it.a > 64'hF000000000000000)) e++;
    if (!(it.b < 64'h1000000000000000 && it.b > 1000)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0h %0h %0h\n", it.a, it.b, it.s);
  endfunction
endclass
