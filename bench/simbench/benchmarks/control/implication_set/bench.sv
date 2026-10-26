// control.implication_set: mode-selected value sets
class sb_item;
  rand bit [2:0] mode;
  rand bit [7:0] len;
  constraint c_i { mode inside {[0:2]}; mode == 0 -> len inside {[1:4]}; mode == 1 -> len inside {8, 16}; mode == 2 -> len >= 8'd100; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.implication_set"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.mode <= 2)) e++;
    if (!(it.mode != 0 || (it.len >= 1 && it.len <= 4))) e++;
    if (!(it.mode != 1 || it.len inside {8,16})) e++;
    if (!(it.mode != 2 || it.len >= 100)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.mode, it.len);
  endfunction
endclass
