// construct.width_mix: mixed widths, slices
class sb_item;
  rand bit [3:0] n;
  rand bit [11:0] m;
  rand bit [39:0] w;
  constraint c_w { m > {8'd0, n}; m < 12'd400; w[39:12] == 28'd5; w[11:0] == m; n inside {[2:12]}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.width_mix"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.m > it.n && it.m < 400)) e++;
    if (!(it.w[39:12] == 5 && it.w[11:0] == it.m)) e++;
    if (!(it.n >= 2 && it.n <= 12)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.n, it.m, it.w);
  endfunction
endclass
