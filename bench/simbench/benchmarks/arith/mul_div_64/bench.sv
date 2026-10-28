// arith.mul_div_64: 64-bit mul/div
class sb_item;
  rand bit [63:0] a, b, p;
  constraint c_m { a inside {[1:64'hFFFF_FFFF]}; b inside {[1:64'hFFFF]}; p == a * b; p / b == a; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.mul_div_64"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.p == it.a*it.b)) e++;
    if (!(it.a >= 1 && it.a <= 64'hFFFFFFFF)) e++;
    if (!(it.b >= 1 && it.b <= 64'hFFFF)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0h %0h %0h\n", it.a, it.b, it.p);
  endfunction
endclass
