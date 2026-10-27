// memmap.memmap_tight_32: 12 ordered regions, sorted sizes, fixed total 192 MB (D test_memmap_tight_32)
class sb_item;
  rand bit [32:0] r0_base;
  rand bit [32:0] r0_size;
  rand bit [32:0] r0_end;
  rand bit [32:0] r1_base;
  rand bit [32:0] r1_size;
  rand bit [32:0] r1_end;
  rand bit [32:0] r2_base;
  rand bit [32:0] r2_size;
  rand bit [32:0] r2_end;
  rand bit [32:0] r3_base;
  rand bit [32:0] r3_size;
  rand bit [32:0] r3_end;
  rand bit [32:0] r4_base;
  rand bit [32:0] r4_size;
  rand bit [32:0] r4_end;
  rand bit [32:0] r5_base;
  rand bit [32:0] r5_size;
  rand bit [32:0] r5_end;
  rand bit [32:0] r6_base;
  rand bit [32:0] r6_size;
  rand bit [32:0] r6_end;
  rand bit [32:0] r7_base;
  rand bit [32:0] r7_size;
  rand bit [32:0] r7_end;
  rand bit [32:0] r8_base;
  rand bit [32:0] r8_size;
  rand bit [32:0] r8_end;
  rand bit [32:0] r9_base;
  rand bit [32:0] r9_size;
  rand bit [32:0] r9_end;
  rand bit [32:0] r10_base;
  rand bit [32:0] r10_size;
  rand bit [32:0] r10_end;
  rand bit [32:0] r11_base;
  rand bit [32:0] r11_size;
  rand bit [32:0] r11_end;
  rand bit [32:0] s01;
  rand bit [32:0] s012;
  rand bit [32:0] s0123;
  rand bit [32:0] s01234;
  rand bit [32:0] s012345;
  rand bit [32:0] s0to6;
  rand bit [32:0] s0to7;
  rand bit [32:0] s0to8;
  rand bit [32:0] s0to9;
  rand bit [32:0] s0to10;
  function bit ok();
    if (!(r0_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r0_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r0_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(r1_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r1_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r1_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(r2_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r2_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r2_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(r3_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r3_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r3_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(r4_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r4_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r4_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(r5_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r5_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r5_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(r6_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r6_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r6_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(r7_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r7_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r7_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(r8_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r8_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r8_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(r9_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r9_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r9_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(r10_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r10_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r10_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(r11_base inside {[33'h0:33'h60000000]})) return 0;
    if (!(r11_size inside {[33'h100000:33'h2000000]})) return 0;
    if (!(r11_end inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(s01 inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(s012 inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(s0123 inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(s01234 inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(s012345 inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(s0to6 inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(s0to7 inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(s0to8 inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(s0to9 inside {[33'h0:33'h7fffffff]})) return 0;
    if (!(s0to10 inside {[33'h0:33'h7fffffff]})) return 0;
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
    if (!(r8_end == r8_base + r8_size)) return 0;
    if (!(r7_end <= r8_base)) return 0;
    if (!(r9_end == r9_base + r9_size)) return 0;
    if (!(r8_end <= r9_base)) return 0;
    if (!(r10_end == r10_base + r10_size)) return 0;
    if (!(r9_end <= r10_base)) return 0;
    if (!(r11_end == r11_base + r11_size)) return 0;
    if (!(r10_end <= r11_base)) return 0;
    if (!(r11_end <= 33'h7fffffff)) return 0;
    if (!(r0_size <= r1_size)) return 0;
    if (!(r1_size <= r2_size)) return 0;
    if (!(r2_size <= r3_size)) return 0;
    if (!(r3_size <= r4_size)) return 0;
    if (!(r4_size <= r5_size)) return 0;
    if (!(r5_size <= r6_size)) return 0;
    if (!(r6_size <= r7_size)) return 0;
    if (!(r7_size <= r8_size)) return 0;
    if (!(r8_size <= r9_size)) return 0;
    if (!(r9_size <= r10_size)) return 0;
    if (!(r10_size <= r11_size)) return 0;
    if (!(s01 == r0_size + r1_size)) return 0;
    if (!(s012 == s01 + r2_size)) return 0;
    if (!(s0123 == s012 + r3_size)) return 0;
    if (!(s01234 == s0123 + r4_size)) return 0;
    if (!(s012345 == s01234 + r5_size)) return 0;
    if (!(s0to6 == s012345 + r6_size)) return 0;
    if (!(s0to7 == s0to6 + r7_size)) return 0;
    if (!(s0to8 == s0to7 + r8_size)) return 0;
    if (!(s0to9 == s0to8 + r9_size)) return 0;
    if (!(s0to10 == s0to9 + r10_size)) return 0;
    if (!(s0to10 + r11_size == 33'd201326592)) return 0;
    return 1;
  endfunction
  constraint c_dom { r0_base inside {[33'h0:33'h60000000]}; r0_size inside {[33'h100000:33'h2000000]}; r0_end inside {[33'h0:33'h7fffffff]}; r1_base inside {[33'h0:33'h60000000]}; r1_size inside {[33'h100000:33'h2000000]}; r1_end inside {[33'h0:33'h7fffffff]}; r2_base inside {[33'h0:33'h60000000]}; r2_size inside {[33'h100000:33'h2000000]}; r2_end inside {[33'h0:33'h7fffffff]}; r3_base inside {[33'h0:33'h60000000]}; r3_size inside {[33'h100000:33'h2000000]}; r3_end inside {[33'h0:33'h7fffffff]}; r4_base inside {[33'h0:33'h60000000]}; r4_size inside {[33'h100000:33'h2000000]}; r4_end inside {[33'h0:33'h7fffffff]}; r5_base inside {[33'h0:33'h60000000]}; r5_size inside {[33'h100000:33'h2000000]}; r5_end inside {[33'h0:33'h7fffffff]}; r6_base inside {[33'h0:33'h60000000]}; r6_size inside {[33'h100000:33'h2000000]}; r6_end inside {[33'h0:33'h7fffffff]}; r7_base inside {[33'h0:33'h60000000]}; r7_size inside {[33'h100000:33'h2000000]}; r7_end inside {[33'h0:33'h7fffffff]}; r8_base inside {[33'h0:33'h60000000]}; r8_size inside {[33'h100000:33'h2000000]}; r8_end inside {[33'h0:33'h7fffffff]}; r9_base inside {[33'h0:33'h60000000]}; r9_size inside {[33'h100000:33'h2000000]}; r9_end inside {[33'h0:33'h7fffffff]}; r10_base inside {[33'h0:33'h60000000]}; r10_size inside {[33'h100000:33'h2000000]}; r10_end inside {[33'h0:33'h7fffffff]}; r11_base inside {[33'h0:33'h60000000]}; r11_size inside {[33'h100000:33'h2000000]}; r11_end inside {[33'h0:33'h7fffffff]}; s01 inside {[33'h0:33'h7fffffff]}; s012 inside {[33'h0:33'h7fffffff]}; s0123 inside {[33'h0:33'h7fffffff]}; s01234 inside {[33'h0:33'h7fffffff]}; s012345 inside {[33'h0:33'h7fffffff]}; s0to6 inside {[33'h0:33'h7fffffff]}; s0to7 inside {[33'h0:33'h7fffffff]}; s0to8 inside {[33'h0:33'h7fffffff]}; s0to9 inside {[33'h0:33'h7fffffff]}; s0to10 inside {[33'h0:33'h7fffffff]}; }
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
  constraint c_end8 { r8_end == r8_base + r8_size; }
  constraint c_order8 { r7_end <= r8_base; }
  constraint c_end9 { r9_end == r9_base + r9_size; }
  constraint c_order9 { r8_end <= r9_base; }
  constraint c_end10 { r10_end == r10_base + r10_size; }
  constraint c_order10 { r9_end <= r10_base; }
  constraint c_end11 { r11_end == r11_base + r11_size; }
  constraint c_order11 { r10_end <= r11_base; }
  constraint c_fits { r11_end <= 33'h7fffffff; }
  constraint c_so1 { r0_size <= r1_size; }
  constraint c_so2 { r1_size <= r2_size; }
  constraint c_so3 { r2_size <= r3_size; }
  constraint c_so4 { r3_size <= r4_size; }
  constraint c_so5 { r4_size <= r5_size; }
  constraint c_so6 { r5_size <= r6_size; }
  constraint c_so7 { r6_size <= r7_size; }
  constraint c_so8 { r7_size <= r8_size; }
  constraint c_so9 { r8_size <= r9_size; }
  constraint c_so10 { r9_size <= r10_size; }
  constraint c_so11 { r10_size <= r11_size; }
  constraint c_s01 { s01 == r0_size + r1_size; }
  constraint c_s012 { s012 == s01 + r2_size; }
  constraint c_s0123 { s0123 == s012 + r3_size; }
  constraint c_s01234 { s01234 == s0123 + r4_size; }
  constraint c_s012345 { s012345 == s01234 + r5_size; }
  constraint c_s0to6 { s0to6 == s012345 + r6_size; }
  constraint c_s0to7 { s0to7 == s0to6 + r7_size; }
  constraint c_s0to8 { s0to8 == s0to7 + r8_size; }
  constraint c_s0to9 { s0to9 == s0to8 + r9_size; }
  constraint c_s0to10 { s0to10 == s0to9 + r10_size; }
  constraint c_total { s0to10 + r11_size == 33'd201326592; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("memmap.memmap_tight_32"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d\n", it.r0_base, it.r0_size, it.r0_end, it.r1_base, it.r1_size, it.r1_end, it.r2_base, it.r2_size, it.r2_end, it.r3_base, it.r3_size, it.r3_end, it.r4_base, it.r4_size, it.r4_end, it.r5_base, it.r5_size, it.r5_end, it.r6_base, it.r6_size, it.r6_end, it.r7_base, it.r7_size, it.r7_end, it.r8_base, it.r8_size, it.r8_end, it.r9_base, it.r9_size, it.r9_end, it.r10_base, it.r10_size, it.r10_end, it.r11_base, it.r11_size, it.r11_end, it.s01, it.s012, it.s0123, it.s01234, it.s012345, it.s0to6, it.s0to7, it.s0to8, it.s0to9, it.s0to10);
  endfunction
endclass
