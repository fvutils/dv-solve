// arith.pow2: power-of-two
class sb_item;
  rand bit [31:0] v;
  rand bit [4:0] k;
  constraint c_p { v != 0; (v & (v - 1)) == 0; v == (32'd1 << k); k >= 5'd4; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.pow2"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.v != 0 && (it.v & (it.v-1)) == 0)) e++;
    if (!(it.v == (32'd1 << it.k))) e++;
    if (!(it.k >= 4)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.v, it.k);
  endfunction
endclass
