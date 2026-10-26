// control.implication_chain_8: chain of 7 implications
class sb_item;
  rand bit [7:0] f;
  rand bit [3:0] k;
  constraint c_i { f[0] -> f[1]; f[1] -> f[2]; f[2] -> f[3]; f[3] -> f[4]; f[4] -> f[5]; f[5] -> f[6]; f[6] -> f[7]; f != 0; $countones(f) == k; k inside {[2:5]}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.implication_chain_8"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(!it.f[0] || it.f[1])) e++;
    if (!(!it.f[1] || it.f[2])) e++;
    if (!(!it.f[2] || it.f[3])) e++;
    if (!(!it.f[3] || it.f[4])) e++;
    if (!(!it.f[4] || it.f[5])) e++;
    if (!(!it.f[5] || it.f[6])) e++;
    if (!(!it.f[6] || it.f[7])) e++;
    if (!(it.f != 0)) e++;
    if (!($countones(it.f) == it.k)) e++;
    if (!(it.k >= 2 && it.k <= 5)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0h %0d\n", it.f, it.k);
  endfunction
endclass
