// array.assoc_basic: associative array with fixed keys
class sb_item;
  rand bit [7:0] m[int];
  function new(); for (int i = 0; i < 5; i++) m[i*10] = 0; endfunction
  function bit ok();
    if (m.num() != 5) return 0;
    foreach (m[k]) if (m[k] != 8'(k + 1)) return 0;
    return 1;
  endfunction
  constraint c_m { foreach (m[k]) m[k] == 8'(k + 1); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.assoc_basic"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%p\n", it.m);
  endfunction
endclass
