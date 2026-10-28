// arith.verilator_operators: bitwise operator mix (cf. t_constraint_operators)
class sb_item;
  rand bit [7:0] a, b, c, d, e;
  constraint c_o { (a & b) == 8'h0F; (a | c) == 8'hFF; (a ^ d) == 8'h55; e == ~a; b != a; c != 8'hFF; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.verilator_operators"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!((it.a & it.b) == 8'h0F)) e++;
    if (!((it.a | it.c) == 8'hFF)) e++;
    if (!((it.a ^ it.d) == 8'h55)) e++;
    if (!(it.e == ~it.a)) e++;
    if (!(it.b != it.a && it.c != 8'hFF)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d\n", it.a, it.b, it.c, it.d, it.e);
  endfunction
endclass
