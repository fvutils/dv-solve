// array.cls_arr_member: array of rand class handles
class sb_elem;
  rand bit [7:0] v;
  constraint c_v { v < 8'd100; }
endclass
class sb_item;
  rand sb_elem e[4];
  function new(); foreach (e[i]) e[i] = new(); endfunction
  function bit ok();
    foreach (e[i]) if (e[i].v >= 100 || (i > 0 && e[i].v <= e[i-1].v)) return 0;
    return 1;
  endfunction
  constraint c_o { foreach (e[i]) { e[i].v < 8'd100; if (i > 0) e[i].v > e[i-1].v; } }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.cls_arr_member"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d\n", it.e[0].v, it.e[1].v, it.e[2].v, it.e[3].v);
  endfunction
endclass
