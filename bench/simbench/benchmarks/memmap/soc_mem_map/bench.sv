// memmap.soc_mem_map: boot/sram/periph/dma regions ordered in 64 KB (D test_soc_mem_map)
class sb_item;
  rand bit [16:0] r0_base;
  rand bit [16:0] r0_size;
  rand bit [16:0] r0_end;
  rand bit [16:0] r1_base;
  rand bit [16:0] r1_size;
  rand bit [16:0] r1_end;
  rand bit [16:0] r2_base;
  rand bit [16:0] r2_size;
  rand bit [16:0] r2_end;
  rand bit [16:0] r3_base;
  rand bit [16:0] r3_size;
  rand bit [16:0] r3_end;
  function bit ok();
    if (!(r0_base inside {[17'h0:17'h9fff]})) return 0;
    if (!(r0_size inside {[17'h1000:17'h6000]})) return 0;
    if (!(r0_end inside {[17'h1000:17'hffff]})) return 0;
    if (!(r1_base inside {[17'h0:17'h9fff]})) return 0;
    if (!(r1_size inside {[17'h1000:17'h6000]})) return 0;
    if (!(r1_end inside {[17'h1000:17'hffff]})) return 0;
    if (!(r2_base inside {[17'h0:17'h9fff]})) return 0;
    if (!(r2_size inside {[17'h1000:17'h6000]})) return 0;
    if (!(r2_end inside {[17'h1000:17'hffff]})) return 0;
    if (!(r3_base inside {[17'h0:17'h9fff]})) return 0;
    if (!(r3_size inside {[17'h1000:17'h6000]})) return 0;
    if (!(r3_end inside {[17'h1000:17'hffff]})) return 0;
    if (!(r0_end == r0_base + r0_size)) return 0;
    if (!(r1_end == r1_base + r1_size)) return 0;
    if (!(r0_end <= r1_base)) return 0;
    if (!(r2_end == r2_base + r2_size)) return 0;
    if (!(r1_end <= r2_base)) return 0;
    if (!(r3_end == r3_base + r3_size)) return 0;
    if (!(r2_end <= r3_base)) return 0;
    if (!(r3_end <= 17'hffff)) return 0;
    return 1;
  endfunction
  constraint c_dom { r0_base inside {[17'h0:17'h9fff]}; r0_size inside {[17'h1000:17'h6000]}; r0_end inside {[17'h1000:17'hffff]}; r1_base inside {[17'h0:17'h9fff]}; r1_size inside {[17'h1000:17'h6000]}; r1_end inside {[17'h1000:17'hffff]}; r2_base inside {[17'h0:17'h9fff]}; r2_size inside {[17'h1000:17'h6000]}; r2_end inside {[17'h1000:17'hffff]}; r3_base inside {[17'h0:17'h9fff]}; r3_size inside {[17'h1000:17'h6000]}; r3_end inside {[17'h1000:17'hffff]}; }
  constraint c_end0 { r0_end == r0_base + r0_size; }
  constraint c_end1 { r1_end == r1_base + r1_size; }
  constraint c_order1 { r0_end <= r1_base; }
  constraint c_end2 { r2_end == r2_base + r2_size; }
  constraint c_order2 { r1_end <= r2_base; }
  constraint c_end3 { r3_end == r3_base + r3_size; }
  constraint c_order3 { r2_end <= r3_base; }
  constraint c_fits { r3_end <= 17'hffff; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("memmap.soc_mem_map"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d\n", it.r0_base, it.r0_size, it.r0_end, it.r1_base, it.r1_size, it.r1_end, it.r2_base, it.r2_size, it.r2_end, it.r3_base, it.r3_size, it.r3_end);
  endfunction
endclass
