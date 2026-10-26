// dist.dist_range: dist over ranges with := weights
class sb_item;
  rand bit [7:0] a;
  rand bit [15:0] b;
  constraint c_d { a dist {[0:9] := 5, [10:99] := 1, [100:255] := 1}; b dist {16'd0 := 1, [16'd1:16'd1000] := 8, 16'hffff := 1}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("dist.dist_range"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(1)) e++;
    if (!(it.b == 0 || it.b <= 1000 || it.b == 16'hffff)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.a, it.b);
  endfunction
endclass
