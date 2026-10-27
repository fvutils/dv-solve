// array.unique_dyn: unique over dynamic array
class sb_item;
  rand bit [7:0] d[];
  function bit ok();
    if (d.size() < 4 || d.size() > 10) return 0;
    foreach (d[i]) begin if (d[i] >= 16) return 0; for (int j = 0; j < i; j++) if (d[j] == d[i]) return 0; end
    return 1;
  endfunction
  constraint c_u { d.size() inside {[4:10]}; unique {d}; foreach (d[i]) d[i] < 8'd16; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.unique_dyn"); endfunction
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
