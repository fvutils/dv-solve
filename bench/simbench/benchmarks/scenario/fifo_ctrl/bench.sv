// scenario.fifo_ctrl: FIFO pointers and watermarks (D test_fifo_ctrl)
class sb_item;
  rand bit [7:0] wr_ptr;
  rand bit [7:0] rd_ptr;
  rand bit [5:0] watermark_lo;
  rand bit [7:0] watermark_hi;
  function bit ok();
    if (!(watermark_hi >= 8'd192)) return 0;
    if (!(wr_ptr != rd_ptr)) return 0;
    if (!(watermark_lo < watermark_hi)) return 0;
    return 1;
  endfunction
  constraint c_dom { watermark_hi >= 8'd192; }
  constraint c_ptrs_differ { wr_ptr != rd_ptr; }
  constraint c_watermark { watermark_lo < watermark_hi; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.fifo_ctrl"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d\n", it.wr_ptr, it.rd_ptr, it.watermark_lo, it.watermark_hi);
  endfunction
endclass
