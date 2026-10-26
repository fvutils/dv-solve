// dist.dist_if: dist under if/else
class sb_item;
  rand bit mode;
  rand bit [7:0] len;
  constraint c_d { if (mode) len dist {[1:4] := 3, [5:64] := 1}; else len dist {[64:128] := 1, [129:255] := 1}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("dist.dist_if"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(!it.mode || (it.len >= 1 && it.len <= 64))) e++;
    if (!(it.mode || it.len >= 64)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.mode, it.len);
  endfunction
endclass
