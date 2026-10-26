// control.solve_before_array: solve scalar before array
class sb_item;
  rand bit [3:0] n;
  rand bit [7:0] arr[8];
  constraint c_s { n >= 4'd2; foreach (arr[i]) arr[i] <= n; solve n before arr; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.solve_before_array"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.n >= 2)) e++;
    if (!(it.arr[0] <= it.n)) e++;
    if (!(it.arr[1] <= it.n)) e++;
    if (!(it.arr[2] <= it.n)) e++;
    if (!(it.arr[3] <= it.n)) e++;
    if (!(it.arr[4] <= it.n)) e++;
    if (!(it.arr[5] <= it.n)) e++;
    if (!(it.arr[6] <= it.n)) e++;
    if (!(it.arr[7] <= it.n)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d %0d\n", it.n, it.arr[0], it.arr[1], it.arr[2], it.arr[3], it.arr[4], it.arr[5], it.arr[6], it.arr[7]);
  endfunction
endclass
