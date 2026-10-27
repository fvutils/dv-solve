// arith.sysfunc: $clog2 relation encoded with ?: (checked with $clog2)
class sb_item;
  rand bit [15:0] n;
  rand bit [4:0] lg;
  constraint c_s { n inside {[2:1024]}; lg == 5'(n - 1 < 2 ? 1 : (n - 1 < 4 ? 2 : (n - 1 < 8 ? 3 : (n - 1 < 16 ? 4 : 5)))); n < 17; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.sysfunc"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.n >= 2 && it.n < 17)) e++;
    if (!(it.lg == 5'($clog2(it.n)))) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.n, it.lg);
  endfunction
endclass
