// arith.unsigned_ops: unsigned compare/subtract
class sb_item;
  rand int unsigned a, b;
  rand bit [15:0] c;
  constraint c_u { a > 32'h8000_0000; b < a; a - b < 32'd1000; c == 16'(a ^ b); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.unsigned_ops"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a > 32'h80000000)) e++;
    if (!(it.b < it.a && it.a-it.b < 1000)) e++;
    if (!(it.c == (it.a[15:0] ^ it.b[15:0]))) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.a, it.b, it.c);
  endfunction
endclass
