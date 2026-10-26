// arith.alignment: natural alignment via mask
class sb_item;
  rand bit [31:0] addr, len;
  constraint c_a { len inside {1, 2, 4, 8, 16, 32, 64}; (addr & (len - 1)) == 0; addr + len <= 32'h2000; addr >= 32'h1000; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.alignment"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.len inside {1,2,4,8,16,32,64})) e++;
    if (!((it.addr & (it.len-1)) == 0)) e++;
    if (!(it.addr+it.len <= 32'h2000 && it.addr >= 32'h1000)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.addr, it.len);
  endfunction
endclass
