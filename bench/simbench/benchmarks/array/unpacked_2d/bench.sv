// array.unpacked_2d: symmetric 2-D matrix, zero diagonal
class sb_item;
  rand bit [3:0] m[4][4];
  function bit ok();
    foreach (m[i,j]) begin if ((i == j) != (m[i][j] == 0)) return 0; if (m[i][j] != m[j][i]) return 0; end
    return 1;
  endfunction
  constraint c_m { foreach (m[i,j]) { (i == j) -> m[i][j] == 0; (i != j) -> m[i][j] != 0; m[i][j] == m[j][i]; } }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.unpacked_2d"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%p\n", it.m);
  endfunction
endclass
