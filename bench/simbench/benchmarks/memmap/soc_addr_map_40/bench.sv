// memmap.soc_addr_map_40: 8 ordered regions of 1-8 GB in 1 TB (D test_soc_addr_map_40)
class sb_item;
  rand bit [40:0] r0_base;
  rand bit [40:0] r0_size;
  rand bit [40:0] r0_end;
  rand bit [40:0] r1_base;
  rand bit [40:0] r1_size;
  rand bit [40:0] r1_end;
  rand bit [40:0] r2_base;
  rand bit [40:0] r2_size;
  rand bit [40:0] r2_end;
  rand bit [40:0] r3_base;
  rand bit [40:0] r3_size;
  rand bit [40:0] r3_end;
  rand bit [40:0] r4_base;
  rand bit [40:0] r4_size;
  rand bit [40:0] r4_end;
  rand bit [40:0] r5_base;
  rand bit [40:0] r5_size;
  rand bit [40:0] r5_end;
  rand bit [40:0] r6_base;
  rand bit [40:0] r6_size;
  rand bit [40:0] r6_end;
  rand bit [40:0] r7_base;
  rand bit [40:0] r7_size;
  rand bit [40:0] r7_end;
  function bit ok();
    if (!(r0_base inside {[41'h0:41'hfdffffffff]})) return 0;
    if (!(r0_size inside {[41'h40000000:41'h200000000]})) return 0;
    if (!(r0_end inside {[41'h40000000:41'hffffffffff]})) return 0;
    if (!(r1_base inside {[41'h0:41'hfdffffffff]})) return 0;
    if (!(r1_size inside {[41'h40000000:41'h200000000]})) return 0;
    if (!(r1_end inside {[41'h40000000:41'hffffffffff]})) return 0;
    if (!(r2_base inside {[41'h0:41'hfdffffffff]})) return 0;
    if (!(r2_size inside {[41'h40000000:41'h200000000]})) return 0;
    if (!(r2_end inside {[41'h40000000:41'hffffffffff]})) return 0;
    if (!(r3_base inside {[41'h0:41'hfdffffffff]})) return 0;
    if (!(r3_size inside {[41'h40000000:41'h200000000]})) return 0;
    if (!(r3_end inside {[41'h40000000:41'hffffffffff]})) return 0;
    if (!(r4_base inside {[41'h0:41'hfdffffffff]})) return 0;
    if (!(r4_size inside {[41'h40000000:41'h200000000]})) return 0;
    if (!(r4_end inside {[41'h40000000:41'hffffffffff]})) return 0;
    if (!(r5_base inside {[41'h0:41'hfdffffffff]})) return 0;
    if (!(r5_size inside {[41'h40000000:41'h200000000]})) return 0;
    if (!(r5_end inside {[41'h40000000:41'hffffffffff]})) return 0;
    if (!(r6_base inside {[41'h0:41'hfdffffffff]})) return 0;
    if (!(r6_size inside {[41'h40000000:41'h200000000]})) return 0;
    if (!(r6_end inside {[41'h40000000:41'hffffffffff]})) return 0;
    if (!(r7_base inside {[41'h0:41'hfdffffffff]})) return 0;
    if (!(r7_size inside {[41'h40000000:41'h200000000]})) return 0;
    if (!(r7_end inside {[41'h40000000:41'hffffffffff]})) return 0;
    if (!(r0_end == r0_base + r0_size)) return 0;
    if (!(r1_end == r1_base + r1_size)) return 0;
    if (!(r0_end <= r1_base)) return 0;
    if (!(r2_end == r2_base + r2_size)) return 0;
    if (!(r1_end <= r2_base)) return 0;
    if (!(r3_end == r3_base + r3_size)) return 0;
    if (!(r2_end <= r3_base)) return 0;
    if (!(r4_end == r4_base + r4_size)) return 0;
    if (!(r3_end <= r4_base)) return 0;
    if (!(r5_end == r5_base + r5_size)) return 0;
    if (!(r4_end <= r5_base)) return 0;
    if (!(r6_end == r6_base + r6_size)) return 0;
    if (!(r5_end <= r6_base)) return 0;
    if (!(r7_end == r7_base + r7_size)) return 0;
    if (!(r6_end <= r7_base)) return 0;
    if (!(r7_end <= 41'hffffffffff)) return 0;
    return 1;
  endfunction
  constraint c_dom { r0_base inside {[41'h0:41'hfdffffffff]}; r0_size inside {[41'h40000000:41'h200000000]}; r0_end inside {[41'h40000000:41'hffffffffff]}; r1_base inside {[41'h0:41'hfdffffffff]}; r1_size inside {[41'h40000000:41'h200000000]}; r1_end inside {[41'h40000000:41'hffffffffff]}; r2_base inside {[41'h0:41'hfdffffffff]}; r2_size inside {[41'h40000000:41'h200000000]}; r2_end inside {[41'h40000000:41'hffffffffff]}; r3_base inside {[41'h0:41'hfdffffffff]}; r3_size inside {[41'h40000000:41'h200000000]}; r3_end inside {[41'h40000000:41'hffffffffff]}; r4_base inside {[41'h0:41'hfdffffffff]}; r4_size inside {[41'h40000000:41'h200000000]}; r4_end inside {[41'h40000000:41'hffffffffff]}; r5_base inside {[41'h0:41'hfdffffffff]}; r5_size inside {[41'h40000000:41'h200000000]}; r5_end inside {[41'h40000000:41'hffffffffff]}; r6_base inside {[41'h0:41'hfdffffffff]}; r6_size inside {[41'h40000000:41'h200000000]}; r6_end inside {[41'h40000000:41'hffffffffff]}; r7_base inside {[41'h0:41'hfdffffffff]}; r7_size inside {[41'h40000000:41'h200000000]}; r7_end inside {[41'h40000000:41'hffffffffff]}; }
  constraint c_end0 { r0_end == r0_base + r0_size; }
  constraint c_end1 { r1_end == r1_base + r1_size; }
  constraint c_order1 { r0_end <= r1_base; }
  constraint c_end2 { r2_end == r2_base + r2_size; }
  constraint c_order2 { r1_end <= r2_base; }
  constraint c_end3 { r3_end == r3_base + r3_size; }
  constraint c_order3 { r2_end <= r3_base; }
  constraint c_end4 { r4_end == r4_base + r4_size; }
  constraint c_order4 { r3_end <= r4_base; }
  constraint c_end5 { r5_end == r5_base + r5_size; }
  constraint c_order5 { r4_end <= r5_base; }
  constraint c_end6 { r6_end == r6_base + r6_size; }
  constraint c_order6 { r5_end <= r6_base; }
  constraint c_end7 { r7_end == r7_base + r7_size; }
  constraint c_order7 { r6_end <= r7_base; }
  constraint c_fits { r7_end <= 41'hffffffffff; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("memmap.soc_addr_map_40"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d\n", it.r0_base, it.r0_size, it.r0_end, it.r1_base, it.r1_size, it.r1_end, it.r2_base, it.r2_size, it.r2_end, it.r3_base, it.r3_size, it.r3_end, it.r4_base, it.r4_size, it.r4_end, it.r5_base, it.r5_size, it.r5_end, it.r6_base, it.r6_size, it.r6_end, it.r7_base, it.r7_size, it.r7_end);
  endfunction
endclass
