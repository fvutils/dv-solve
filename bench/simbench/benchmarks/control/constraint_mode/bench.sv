// control.constraint_mode: constraint_mode(0) disables a conflicting block
class sb_item;
  rand bit [7:0] a;
  constraint c_small { a < 8'd10; }
  constraint c_big { a > 8'd100; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.constraint_mode"); endfunction
  virtual function bit do_randomize(); it.c_small.constraint_mode(0); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a > 100)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d\n", it.a);
  endfunction
endclass
