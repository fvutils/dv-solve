// memmap.mem_partition_knapsack: 8 power-of-2 partitions summing to 2048 with prefix offsets; offsets renamed off<i>: Verilator solver names a<N> clash (D test_mem_partition_knapsack)
class sb_item;
  rand bit [12:0] s0;
  rand bit [12:0] s1;
  rand bit [12:0] s2;
  rand bit [12:0] s3;
  rand bit [12:0] s4;
  rand bit [12:0] s5;
  rand bit [12:0] s6;
  rand bit [12:0] s7;
  rand bit [12:0] off0;
  rand bit [12:0] off1;
  rand bit [12:0] off2;
  rand bit [12:0] off3;
  rand bit [12:0] off4;
  rand bit [12:0] off5;
  rand bit [12:0] off6;
  rand bit [12:0] off7;
  rand bit [12:0] total;
  function bit ok();
    if (!((s0 == 64 || s0 == 128 || s0 == 256 || s0 == 512))) return 0;
    if (!((s1 == 64 || s1 == 128 || s1 == 256 || s1 == 512))) return 0;
    if (!((s2 == 64 || s2 == 128 || s2 == 256 || s2 == 512))) return 0;
    if (!((s3 == 64 || s3 == 128 || s3 == 256 || s3 == 512))) return 0;
    if (!((s4 == 64 || s4 == 128 || s4 == 256 || s4 == 512))) return 0;
    if (!((s5 == 64 || s5 == 128 || s5 == 256 || s5 == 512))) return 0;
    if (!((s6 == 64 || s6 == 128 || s6 == 256 || s6 == 512))) return 0;
    if (!((s7 == 64 || s7 == 128 || s7 == 256 || s7 == 512))) return 0;
    if (!(off0 == 0)) return 0;
    if (!(off1 inside {[64:1984]})) return 0;
    if (!(off2 inside {[128:1984]})) return 0;
    if (!(off3 inside {[192:1984]})) return 0;
    if (!(off4 inside {[256:1984]})) return 0;
    if (!(off5 inside {[320:1984]})) return 0;
    if (!(off6 inside {[384:1984]})) return 0;
    if (!(off7 inside {[448:1984]})) return 0;
    if (!(total == 13'd2048)) return 0;
    if (!(total == s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7)) return 0;
    if (!(off1 == off0 + s0)) return 0;
    if (!(off2 == off1 + s1)) return 0;
    if (!(off3 == off2 + s2)) return 0;
    if (!(off4 == off3 + s3)) return 0;
    if (!(off5 == off4 + s4)) return 0;
    if (!(off6 == off5 + s5)) return 0;
    if (!(off7 == off6 + s6)) return 0;
    return 1;
  endfunction
  constraint c_dom { (s0 == 64 || s0 == 128 || s0 == 256 || s0 == 512); (s1 == 64 || s1 == 128 || s1 == 256 || s1 == 512); (s2 == 64 || s2 == 128 || s2 == 256 || s2 == 512); (s3 == 64 || s3 == 128 || s3 == 256 || s3 == 512); (s4 == 64 || s4 == 128 || s4 == 256 || s4 == 512); (s5 == 64 || s5 == 128 || s5 == 256 || s5 == 512); (s6 == 64 || s6 == 128 || s6 == 256 || s6 == 512); (s7 == 64 || s7 == 128 || s7 == 256 || s7 == 512); off0 == 0; off1 inside {[64:1984]}; off2 inside {[128:1984]}; off3 inside {[192:1984]}; off4 inside {[256:1984]}; off5 inside {[320:1984]}; off6 inside {[384:1984]}; off7 inside {[448:1984]}; total == 13'd2048; }
  constraint c_sum { total == s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7; }
  constraint c_off1 { off1 == off0 + s0; }
  constraint c_off2 { off2 == off1 + s1; }
  constraint c_off3 { off3 == off2 + s2; }
  constraint c_off4 { off4 == off3 + s3; }
  constraint c_off5 { off5 == off4 + s4; }
  constraint c_off6 { off6 == off5 + s5; }
  constraint c_off7 { off7 == off6 + s6; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("memmap.mem_partition_knapsack"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d\n", it.s0, it.s1, it.s2, it.s3, it.s4, it.s5, it.s6, it.s7, it.off0, it.off1, it.off2, it.off3, it.off4, it.off5, it.off6, it.off7, it.total);
  endfunction
endclass
