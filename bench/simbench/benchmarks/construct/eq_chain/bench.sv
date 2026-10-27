// construct.eq_chain: chained equalities
class sb_item;
  rand bit [15:0] a, b, c, d;
  constraint c_eq { b == a + 16'd3; c == b + 16'd5; d == c - 16'd1; a < 16'd1000; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.eq_chain"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.b == it.a+3)) e++;
    if (!(it.c == it.b+5)) e++;
    if (!(it.d == it.c-1)) e++;
    if (!(it.a < 1000)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d\n", it.a, it.b, it.c, it.d);
  endfunction
endclass
