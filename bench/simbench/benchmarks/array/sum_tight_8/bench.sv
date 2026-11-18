// array.sum_tight_8: sum() == K near the top of its range; 8 x 8-bit elements, each < 64
class sb_item;
  rand bit [7:0] a[8];
  function bit ok();
    int s = 0;
    foreach (a[i]) begin if (a[i] >= 64) return 0; s += a[i]; end
    if (!(s == 480)) return 0;
    return 1;
  endfunction
  constraint c_e { foreach (a[i]) a[i] < 64; }
  constraint c_s { a.sum() with (int'(item)) == 480; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.sum_tight_8"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%p\n", it.a);
  endfunction
endclass
