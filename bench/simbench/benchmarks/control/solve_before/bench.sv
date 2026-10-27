// control.solve_before: solve-before ordering
class sb_item;
  rand bit s;
  rand bit [7:0] v;
  constraint c_s { s -> v == 0; solve s before v; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.solve_before"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(!it.s || it.v == 0)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.s, it.v);
  endfunction
endclass
