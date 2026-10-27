// dist.randc_basic: randc cyclic variables
class sb_item;
  randc bit [3:0] a;
  randc bit [5:0] b;
  rand bit [7:0] c;
  constraint c_r { b < 6'd40; c > {4'd0, a}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("dist.randc_basic"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.b < 40)) e++;
    if (!(it.c > it.a)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.a, it.b, it.c);
  endfunction
endclass
