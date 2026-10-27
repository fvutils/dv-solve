// arith.redops: reduction operators
class sb_item;
  rand bit [7:0] a, b, c;
  constraint c_r { ^a == 1'b1; &b[3:0] == 1'b1; |c == 1'b1; ~|c[7:4]; b[7:4] != 4'hF; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.redops"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(^it.a == 1)) e++;
    if (!(&it.b[3:0] == 1)) e++;
    if (!(|it.c == 1 && it.c[7:4] == 0)) e++;
    if (!(it.b[7:4] != 4'hF)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.a, it.b, it.c);
  endfunction
endclass
