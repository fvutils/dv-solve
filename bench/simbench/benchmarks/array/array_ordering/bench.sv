// array.array_ordering: zigzag ordering between neighbours
class sb_item;
  rand bit [7:0] a[8];
  function bit ok();
    for (int i = 0; i < 7; i++) if ((i % 2 == 0) ? !(a[i] < a[i+1]) : !(a[i] > a[i+1])) return 0;
    return 1;
  endfunction
  constraint c_o { foreach (a[i]) if (i < 7) { if (i % 2 == 0) a[i] < a[i+1]; else a[i] > a[i+1]; } }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.array_ordering"); endfunction
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
