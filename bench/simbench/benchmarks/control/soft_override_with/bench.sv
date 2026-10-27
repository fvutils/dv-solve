// control.soft_override_with: inline with overrides soft
class sb_item;
  rand bit [7:0] a;
  constraint c_s { soft a inside {[0:9]}; a < 8'd200; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.soft_override_with"); endfunction
  virtual function bit do_randomize(); return it.randomize() with { a > 8'd100; }; endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a > 100 && it.a < 200)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d\n", it.a);
  endfunction
endclass
