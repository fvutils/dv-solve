// array.array_reduction: xor/or/and array reductions
class sb_item;
  rand bit [7:0] a[6];
  function bit ok();
    bit [7:0] x = 0, o = 0, n = 8'hFF;
    foreach (a[i]) begin x ^= a[i]; o |= a[i]; n &= a[i]; end
    if (x != 8'h5A || o != 8'hFF || n != 0) return 0;
    return 1;
  endfunction
  constraint c_r { a.xor() == 8'h5A; a.or() == 8'hFF; a.and() == 8'h00; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.array_reduction"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%p\n", it.a);
  endfunction
endclass
