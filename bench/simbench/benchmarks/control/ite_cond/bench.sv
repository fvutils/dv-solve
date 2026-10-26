// control.ite_cond: if/else-if/else constraint
class sb_item;
  rand bit [1:0] kind;
  rand bit [7:0] a;
  constraint c_i { if (kind == 0) { a < 8'd10; } else if (kind == 1) { a inside {[50:60]}; } else { a > 8'd200; } }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.ite_cond"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.kind != 0 || it.a < 10)) e++;
    if (!(it.kind != 1 || (it.a >= 50 && it.a <= 60))) e++;
    if (!(it.kind < 2 || it.a > 200)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.kind, it.a);
  endfunction
endclass
