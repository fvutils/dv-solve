// scenario.riscv_pmp_cfg: RISC-V PMP entry: R/W rule, NAPOT encoding, TOR alignment
class sb_item;
  rand bit [1:0] a_mode;
  rand bit r;
  rand bit w;
  rand bit x;
  rand bit l;
  rand bit [31:0] pmpaddr;
  rand bit [4:0] napot_g;
  function bit ok();
    if (!(a_mode != 2'd2)) return 0;
    if (!(napot_g inside {[1:20]})) return 0;
    if ((w) && !(r)) return 0;
    if ((a_mode == 2'd3) && !((pmpaddr & ((32'd1 << (napot_g + 1)) - 1)) == ((32'd1 << (napot_g - 1)) - 1))) return 0;
    if ((a_mode == 2'd0) && !(!l)) return 0;
    if ((a_mode == 2'd1) && !(pmpaddr[1:0] == 0 && pmpaddr != 0)) return 0;
    return 1;
  endfunction
  constraint c_dom { a_mode != 2'd2; napot_g inside {[1:20]}; }
  constraint c_wr { (w) -> (r); }
  constraint c_napot { (a_mode == 2'd3) -> ((pmpaddr & ((32'd1 << (napot_g + 1)) - 1)) == ((32'd1 << (napot_g - 1)) - 1)); }
  constraint c_off { (a_mode == 2'd0) -> (!l); }
  constraint c_tor { (a_mode == 2'd1) -> (pmpaddr[1:0] == 0 && pmpaddr != 0); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.riscv_pmp_cfg"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d\n", it.a_mode, it.r, it.w, it.x, it.l, it.pmpaddr, it.napot_g);
  endfunction
endclass
