// arith.bitslice: bit-slice equalities
class sb_item;
  rand bit [31:0] w;
  rand bit [3:0] hi, lo;
  constraint c_b { w[31:28] == hi; w[3:0] == lo; hi > lo; w[15:8] == {hi, lo}; w[27:16] == 12'hABC; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.bitslice"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.w[31:28] == it.hi && it.w[3:0] == it.lo)) e++;
    if (!(it.hi > it.lo)) e++;
    if (!(it.w[15:8] == {it.hi,it.lo})) e++;
    if (!(it.w[27:16] == 12'hABC)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0h %0h %0h\n", it.w, it.hi, it.lo);
  endfunction
endclass
