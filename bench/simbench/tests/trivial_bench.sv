class sb_item; rand bit [7:0] x; constraint c { x < 10; } endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("trivial"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check(); return (it.x < 10) ? 0 : 1; endfunction
  virtual function void dump(int fd); $fwrite(fd, "%0d\n", it.x); endfunction
endclass
