// arith.countones: $countones
class sb_item;
  rand bit [15:0] v;
  rand bit [31:0] w;
  constraint c_c { $countones(v) == 5; $countones(w) inside {[1:3]}; w[0] == 1'b0; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.countones"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!($countones(it.v) == 5)) e++;
    if (!($countones(it.w) >= 1 && $countones(it.w) <= 3)) e++;
    if (!(it.w[0] == 0)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0h %0h\n", it.v, it.w);
  endfunction
endclass
