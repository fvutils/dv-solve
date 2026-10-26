// arith.shift_width: variable shift into wider result
class sb_item;
  rand bit [15:0] a;
  rand bit [4:0] k;
  rand bit [31:0] r;
  constraint c_s { r == ({16'd0, a} << k); k inside {[1:16]}; r[31:16] != 0; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.shift_width"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.r == ({16'd0,it.a} << it.k))) e++;
    if (!(it.k >= 1 && it.k <= 16)) e++;
    if (!(it.r[31:16] != 0)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.a, it.k, it.r);
  endfunction
endclass
