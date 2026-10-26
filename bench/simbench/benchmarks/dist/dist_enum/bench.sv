// dist.dist_enum: dist over enum values
typedef enum bit [1:0] {K_A, K_B, K_C} kind_e;
class sb_item;
  rand kind_e k;
  rand bit [7:0] n;
  constraint c_d { k dist {K_A := 6, K_B := 3, K_C := 1}; (k == K_C) -> n > 8'd200; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("dist.dist_enum"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.k != K_C || it.n > 200)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.k, it.n);
  endfunction
endclass
