// array.array_sum: array sum (element width)
class sb_item;
  rand bit [15:0] a[8];
  function bit ok();
    int s = 0;
    foreach (a[i]) begin if (a[i] >= 1000) return 0; s += a[i]; end
    if (s != 3000) return 0;
    return 1;
  endfunction
  constraint c_s { foreach (a[i]) a[i] < 16'd1000; a.sum() == 16'd3000; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.array_sum"); endfunction
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
