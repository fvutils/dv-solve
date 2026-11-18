// array.sum_range_32: sum() in a band around the mean; 32 x 8-bit elements, each < 64
class sb_item;
  rand bit [7:0] a[32];
  function bit ok();
    int s = 0;
    foreach (a[i]) begin if (a[i] >= 64) return 0; s += a[i]; end
    if (!(s inside {[768:1280]})) return 0;
    return 1;
  endfunction
  constraint c_e { foreach (a[i]) a[i] < 64; }
  constraint c_s { a.sum() with (int'(item)) inside {[768:1280]}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.sum_range_32"); endfunction
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
