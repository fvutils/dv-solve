// dist.dist_weight_div: dist with :/ weights
class sb_item;
  rand bit [7:0] a;
  rand bit [3:0] k;
  constraint c_d { a dist {[0:15] :/ 4, [16:255] :/ 1}; k dist {0 :/ 1, [1:14] :/ 7, 15 :/ 2}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("dist.dist_weight_div"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(1)) e++;
    if (!(1)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.a, it.k);
  endfunction
endclass
