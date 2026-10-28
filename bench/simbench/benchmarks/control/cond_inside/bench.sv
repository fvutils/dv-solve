// control.cond_inside: inside as implication condition
class sb_item;
  rand bit [1:0] sel;
  rand bit [7:0] v;
  constraint c_c { sel != 3; (sel inside {0, 1}) -> v inside {[0:15]}; !(sel inside {0, 1}) -> v inside {[128:255]}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.cond_inside"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.sel != 3)) e++;
    if (!(!(it.sel inside {0,1}) || it.v <= 15)) e++;
    if (!((it.sel inside {0,1}) || it.v >= 128)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.sel, it.v);
  endfunction
endclass
