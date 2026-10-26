// construct.cmp_web: web of inequalities (cf. dv-solve inequality_web)
class sb_item;
  rand bit [15:0] a, b, c, d, e, f;
  constraint c_w { a < b; b < c; c < d; a + 16'd10 < e; e < d; f > b; f < e; d < 16'd5000; a > 16'd100; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.cmp_web"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a < it.b && it.b < it.c && it.c < it.d)) e++;
    if (!(it.a+10 < it.e && it.e < it.d)) e++;
    if (!(it.f > it.b && it.f < it.e)) e++;
    if (!(it.d < 5000 && it.a > 100)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d\n", it.a, it.b, it.c, it.d, it.e, it.f);
  endfunction
endclass
