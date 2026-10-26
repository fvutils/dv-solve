// arith.mod_align: modulus constraints
class sb_item;
  rand bit [31:0] a;
  rand bit [7:0] m;
  constraint c_m { m inside {3, 5, 7, 12}; a % m == 0; a % 32'd16 == 32'd4 * (m % 4); a < 32'h10_0000; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.mod_align"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.m inside {3,5,7,12})) e++;
    if (!(it.a % it.m == 0)) e++;
    if (!(it.a % 16 == 4*(it.m%4))) e++;
    if (!(it.a < 32'h100000)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.a, it.m);
  endfunction
endclass
