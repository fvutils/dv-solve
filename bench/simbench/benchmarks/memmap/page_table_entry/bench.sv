// memmap.page_table_entry: RISC-V Sv39 PTE: leaf/pointer rules and superpage alignment
class sb_item;
  rand bit [1:0] level;
  rand bit v;
  rand bit r;
  rand bit w;
  rand bit x;
  rand bit u;
  rand bit g;
  rand bit a;
  rand bit d;
  rand bit [1:0] rsw;
  rand bit [43:0] ppn;
  function bit ok();
    if (!(level <= 2'd2)) return 0;
    if (!(v == 1)) return 0;
    if ((w) && !(r)) return 0;
    if ((r || x) && !(a)) return 0;
    if ((w) && !(d)) return 0;
    if ((!r && !x) && !(!u && !a && !d && level != 0)) return 0;
    if (((r || x) && level == 1) && !(ppn[8:0] == 0)) return 0;
    if (((r || x) && level == 2) && !(ppn[17:0] == 0)) return 0;
    return 1;
  endfunction
  constraint c_dom { level <= 2'd2; v == 1; }
  constraint c_wr { (w) -> (r); }
  constraint c_leaf_a { (r || x) -> (a); }
  constraint c_w_d { (w) -> (d); }
  constraint c_ptr { (!r && !x) -> (!u && !a && !d && level != 0); }
  constraint c_mega { ((r || x) && level == 1) -> (ppn[8:0] == 0); }
  constraint c_giga { ((r || x) && level == 2) -> (ppn[17:0] == 0); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("memmap.page_table_entry"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d\n", it.level, it.v, it.r, it.w, it.x, it.u, it.g, it.a, it.d, it.rsw, it.ppn);
  endfunction
endclass
