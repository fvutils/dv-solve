// array.sum_partition: two arrays with equal sums
class sb_item;
  rand bit [7:0] a[4], b[4];
  function bit ok();
    int sa = 0, sb = 0;
    foreach (a[i]) begin sa += a[i]; sb += b[i]; end
    if (sa != sb || sa + sb != 400) return 0;
    return 1;
  endfunction
  constraint c_p { a.sum() with (int'(item)) == b.sum() with (int'(item)); a.sum() with (int'(item)) + b.sum() with (int'(item)) == 400; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.sum_partition"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%p %p\n", it.a, it.b);
  endfunction
endclass
