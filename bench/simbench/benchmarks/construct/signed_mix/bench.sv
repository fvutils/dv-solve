// construct.signed_mix: signed/unsigned mix
class sb_item;
  rand int a;
  rand byte b;
  rand shortint c;
  rand bit [7:0] u;
  constraint c_s { a > -1000; a < 1000; b < 0; b > -100; c == a - b; u > 8'd100; int'(u) + a > 0; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.signed_mix"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a > -1000 && it.a < 1000)) e++;
    if (!(it.b < 0 && it.b > -100)) e++;
    if (!(it.c == it.a - it.b)) e++;
    if (!(it.u > 100 && int'(it.u)+it.a > 0)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d\n", it.a, it.b, it.c, it.u);
  endfunction
endclass
