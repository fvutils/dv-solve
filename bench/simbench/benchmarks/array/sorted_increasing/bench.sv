// array.sorted_increasing: strictly increasing dynamic array
class sb_item;
  rand int unsigned d[];
  function bit ok();
    if (d.size() < 4 || d.size() > 12) return 0;
    foreach (d[i]) if (d[i] >= 1000 || (i > 0 && d[i] < d[i-1] + 2)) return 0;
    return 1;
  endfunction
  constraint c_s { d.size() inside {[4:12]}; foreach (d[i]) { d[i] < 1000; if (i > 0) d[i] >= d[i-1] + 2; } }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.sorted_increasing"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%p\n", it.d);
  endfunction
endclass
