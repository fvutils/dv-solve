// array.dyn_size: dynamic size tied to rand var
class sb_item;
  rand bit [7:0] d[];
  rand bit [3:0] n;
  function bit ok();
    if (d.size() != n || n < 1 || n > 12) return 0;
    foreach (d[i]) if (d[i] != i*2) return 0;
    return 1;
  endfunction
  constraint c_d { n inside {[1:12]}; d.size() == n; foreach (d[i]) d[i] == i*2; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.dyn_size"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %p\n", it.n, it.d);
  endfunction
endclass
