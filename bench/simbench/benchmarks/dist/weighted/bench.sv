// dist.weighted: dist with := (per-value) and :/ (per-range) weights.
// check() checks only the legal ranges; distribution shape is checked in Python
// from the dump.
class sb_item;
  rand bit [7:0] v;   // := weights
  rand bit [7:0] r;   // :/ weights
  constraint c_v { v dist { 0 := 1, 1 := 2, 2 := 4, 3 := 8 }; }
  constraint c_r { r dist { [0:9] :/ 1, [10:19] :/ 3 }; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("dist.weighted"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (it.v > 3) e++;
    if (it.r > 19) e++;
    return e;
  endfunction
  virtual function void dump(int fd); $fwrite(fd, "%0d %0d\n", it.v, it.r); endfunction
endclass
