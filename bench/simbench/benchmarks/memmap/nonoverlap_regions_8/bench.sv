// memmap.nonoverlap_regions_8: 8 aligned regions, pairwise non-overlapping, unordered
class sb_item;
  rand bit [31:0] base[8], size[8];
  function bit ok();
    foreach (base[i]) begin
      if (base[i][7:0] != 0 || size[i] < 256 || size[i] > 16384 || base[i] + size[i] > 32'h100000) return 0;
      for (int j = 0; j < i; j++) if (!(base[i] + size[i] <= base[j] || base[j] + size[j] <= base[i])) return 0;
    end
    return 1;
  endfunction
  constraint c_r { foreach (base[i]) { base[i][7:0] == 0; size[i] inside {[256:16384]}; base[i] + size[i] <= 32'h100000; } }
  constraint c_no { foreach (base[i]) foreach (base[j]) if (i < j) { base[i] + size[i] <= base[j] || base[j] + size[j] <= base[i]; } }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("memmap.nonoverlap_regions_8"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%p %p\n", it.base, it.size);
  endfunction
endclass
