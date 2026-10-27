// scenario.bus_transaction: bus burst: end address in range (D test_bus_transaction)
class sb_item;
  rand bit [15:0] word_addr;
  rand bit [7:0] burst_len;
  rand bit [1:0] beat_size;
  rand bit [16:0] end_addr;
  function bit ok();
    if (!(end_addr == word_addr + burst_len)) return 0;
    if (!(end_addr <= 17'd65535)) return 0;
    if (!(beat_size <= burst_len)) return 0;
    return 1;
  endfunction
  constraint c_end_addr { end_addr == word_addr + burst_len; }
  constraint c_in_range { end_addr <= 17'd65535; }
  constraint c_beat_fits { beat_size <= burst_len; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.bus_transaction"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d\n", it.word_addr, it.burst_len, it.beat_size, it.end_addr);
  endfunction
endclass
