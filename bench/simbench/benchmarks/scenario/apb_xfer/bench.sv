// scenario.apb_xfer: APB4 transfer: aligned address window, read strobes zero
class sb_item;
  rand bit [31:0] paddr;
  rand bit pwrite;
  rand bit [31:0] pwdata;
  rand bit [3:0] pstrb;
  rand bit [2:0] pprot;
  function bit ok();
    if (!(paddr[1:0] == 2'b00)) return 0;
    if (!(paddr inside {[32'h4000_0000:32'h4000_FFFF]})) return 0;
    if ((!pwrite) && !(pstrb == 0 && pwdata == 0)) return 0;
    if ((pwrite) && !(pstrb != 0)) return 0;
    if (!(pprot[2] == 0)) return 0;
    return 1;
  endfunction
  constraint c_dom { paddr[1:0] == 2'b00; paddr inside {[32'h4000_0000:32'h4000_FFFF]}; }
  constraint c_rd_strb { (!pwrite) -> (pstrb == 0 && pwdata == 0); }
  constraint c_wr_strb { (pwrite) -> (pstrb != 0); }
  constraint c_prot { pprot[2] == 0; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.apb_xfer"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d\n", it.paddr, it.pwrite, it.pwdata, it.pstrb, it.pprot);
  endfunction
endclass
