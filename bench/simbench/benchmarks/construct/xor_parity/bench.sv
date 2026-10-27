// construct.xor_parity: xor-reduction parity
class sb_item;
  rand bit [31:0] data;
  rand bit p;
  rand bit [7:0] lo;
  constraint c_p { p == ^data; p == 1'b1; lo == data[7:0]; lo[0] == 1'b0; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.xor_parity"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.p == ^it.data)) e++;
    if (!(it.p == 1)) e++;
    if (!(it.lo == it.data[7:0] && it.lo[0] == 0)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.data, it.p, it.lo);
  endfunction
endclass
