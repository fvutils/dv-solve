// dist.dist_mixed: dist mixed with hard constraints
class sb_item;
  rand bit [31:0] addr;
  rand bit [2:0] sz;
  constraint c_d { addr dist {[32'h0:32'hfff] := 1, [32'h1000:32'hffff_ffff] :/ 1}; sz dist {0 := 1, 1 := 1, 2 := 4, 3 := 2}; sz < 3'd4; addr % (1 << sz) == 0; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("dist.dist_mixed"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.sz < 4)) e++;
    if (!(it.addr % (1 << it.sz) == 0)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.addr, it.sz);
  endfunction
endclass
