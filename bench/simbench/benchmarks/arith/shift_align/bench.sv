// arith.shift_align: alignment via shift round-trip
class sb_item;
  rand bit [31:0] addr;
  rand bit [2:0] sz;
  constraint c_s { (addr >> sz) << sz == addr; addr < 32'h1_0000; sz <= 3'd5; addr != 0; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.shift_align"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(((it.addr >> it.sz) << it.sz) == it.addr)) e++;
    if (!(it.addr < 32'h10000 && it.addr != 0)) e++;
    if (!(it.sz <= 5)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.addr, it.sz);
  endfunction
endclass
