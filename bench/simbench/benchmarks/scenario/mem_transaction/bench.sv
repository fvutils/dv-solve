// scenario.mem_transaction: memory transfer without overflow (D test_mem_transaction)
class sb_item;
  rand bit [7:0] addr;
  rand bit [4:0] length;
  rand bit [8:0] end_addr;
  function bit ok();
    if (!(addr <= 8'd252)) return 0;
    if (!(length inside {[1:16]})) return 0;
    if (!(end_addr == addr + length)) return 0;
    if (!(end_addr <= 9'd256)) return 0;
    return 1;
  endfunction
  constraint c_dom { addr <= 8'd252; length inside {[1:16]}; }
  constraint c_end { end_addr == addr + length; }
  constraint c_no_overflow { end_addr <= 9'd256; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.mem_transaction"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.addr, it.length, it.end_addr);
  endfunction
endclass
