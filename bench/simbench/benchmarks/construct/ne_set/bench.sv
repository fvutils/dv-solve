// construct.ne_set: pairwise != over small domain
class sb_item;
  rand bit [3:0] a, b, c, d;
  constraint c_ne { a != b; a != c; a != d; b != c; b != d; c != d; a != 4'd0; d != 4'd15; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.ne_set"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a!=it.b && it.a!=it.c && it.a!=it.d)) e++;
    if (!(it.b!=it.c && it.b!=it.d && it.c!=it.d)) e++;
    if (!(it.a!=0 && it.d!=15)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d\n", it.a, it.b, it.c, it.d);
  endfunction
endclass
