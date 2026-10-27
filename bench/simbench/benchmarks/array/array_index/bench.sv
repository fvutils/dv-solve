// array.array_index: random index into array
class sb_item;
  rand bit [7:0] a[8];
  rand bit [2:0] idx;
  function bit ok();
    if (a[idx] != 8'hAA) return 0;
    foreach (a[i]) if (i != idx && a[i] >= 8'h80) return 0;
    return 1;
  endfunction
  constraint c_i { a[idx] == 8'hAA; foreach (a[i]) (i != idx) -> a[i] < 8'h80; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.array_index"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %p\n", it.idx, it.a);
  endfunction
endclass
