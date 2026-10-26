// array.sum_with: sum() with widening cast
class sb_item;
  rand bit [3:0] a[10];
  function bit ok();
    int s = 0;
    foreach (a[i]) s += a[i];
    if (s != 60) return 0;
    return 1;
  endfunction
  constraint c_s { a.sum() with (int'(item)) == 60; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.sum_with"); endfunction
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
