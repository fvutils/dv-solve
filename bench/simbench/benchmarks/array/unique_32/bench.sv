// array.unique_32: unique over 32 elements in [0:63]
class sb_item;
  rand bit [7:0] a[32];
  function bit ok();
    foreach (a[i]) begin if (a[i] >= 64) return 0; for (int j = 0; j < i; j++) if (a[j] == a[i]) return 0; end
    return 1;
  endfunction
  constraint c_u { unique {a}; foreach (a[i]) a[i] < 8'd64; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.unique_32"); endfunction
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
