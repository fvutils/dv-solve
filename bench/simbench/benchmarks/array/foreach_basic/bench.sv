// array.foreach_basic: per-element range via foreach
class sb_item;
  rand bit [7:0] a[16];
  function bit ok();
    foreach (a[i]) if (a[i] < i*4 || a[i] > i*4+3) return 0;
    return 1;
  endfunction
  constraint c_f { foreach (a[i]) a[i] inside {[i*4:i*4+3]}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.foreach_basic"); endfunction
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
