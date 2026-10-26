// dist.randc_enum: randc over an enum
typedef enum bit [2:0] {RED, GREEN, BLUE, CYAN, MAGENTA, YELLOW} col_e;
class sb_item;
  randc col_e col;
  rand bit [7:0] v;
  constraint c_r { (col == RED) -> v < 8'd50; (col == BLUE) -> v > 8'd200; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("dist.randc_enum"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.col != RED || it.v < 50)) e++;
    if (!(it.col != BLUE || it.v > 200)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.col, it.v);
  endfunction
endclass
