// control.inline_with: randomize() with {...}
class sb_item;
  rand bit [7:0] a, b;
  constraint c_r { a < 8'd250; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.inline_with"); endfunction
  virtual function bit do_randomize(); return it.randomize() with { a == b + 8'd3; b < 8'd50; }; endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a == 8'(it.b + 3))) e++;
    if (!(it.b < 50)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.a, it.b);
  endfunction
endclass
