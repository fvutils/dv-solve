// scenario.ddr5_timing: DDR5 command-to-command spacing (D test_ddr5_timing)
class sb_item;
  rand bit [2:0] cmd_prev;
  rand bit [2:0] cmd;
  rand bit ap_prev;
  rand bit cancel_prev;
  rand bit [3:0] max_tccd;
  rand bit [5:0] tccd;
  rand bit [5:0] tccd_base;
  function bit ok();
    if (!(cmd_prev <= 3'd4)) return 0;
    if (!(cmd <= 3'd4)) return 0;
    if (!(max_tccd <= 4'd8)) return 0;
    if (!(tccd inside {[2:50]})) return 0;
    if (!(tccd_base inside {[2:50]})) return 0;
    if (!(tccd == tccd_base + max_tccd)) return 0;
    if ((cmd_prev == 3'd2 && cmd == 3'd2) && !(tccd_base >= 8)) return 0;
    if ((cmd_prev == 3'd2 && cmd != 3'd2) && !(tccd_base >= 16)) return 0;
    if ((cmd_prev == 3'd0 && cmd == 3'd0) && !(tccd_base >= 8)) return 0;
    if ((cmd_prev == 3'd0 && cmd != 3'd0) && !(tccd_base >= 24)) return 0;
    if ((cmd_prev == 3'd1 && cmd == 3'd1) && !(tccd_base >= 8)) return 0;
    if ((cmd_prev == 3'd1 && cmd == 3'd4) && !(tccd_base >= 12)) return 0;
    if ((cmd_prev == 3'd4 && cmd == 3'd4) && !(tccd_base >= 2)) return 0;
    if ((cmd_prev == 3'd4 && cmd != 3'd4) && !(tccd_base >= 24)) return 0;
    return 1;
  endfunction
  constraint c_dom { cmd_prev <= 3'd4; cmd <= 3'd4; max_tccd <= 4'd8; tccd inside {[2:50]}; tccd_base inside {[2:50]}; }
  constraint c_tccd_sum { tccd == tccd_base + max_tccd; }
  constraint c_mrw_mrw { (cmd_prev == 3'd2 && cmd == 3'd2) -> (tccd_base >= 8); }
  constraint c_mrw_other { (cmd_prev == 3'd2 && cmd != 3'd2) -> (tccd_base >= 16); }
  constraint c_act_act { (cmd_prev == 3'd0 && cmd == 3'd0) -> (tccd_base >= 8); }
  constraint c_act_other { (cmd_prev == 3'd0 && cmd != 3'd0) -> (tccd_base >= 24); }
  constraint c_rd_rd { (cmd_prev == 3'd1 && cmd == 3'd1) -> (tccd_base >= 8); }
  constraint c_rd_pre { (cmd_prev == 3'd1 && cmd == 3'd4) -> (tccd_base >= 12); }
  constraint c_pre_pre { (cmd_prev == 3'd4 && cmd == 3'd4) -> (tccd_base >= 2); }
  constraint c_pre_other { (cmd_prev == 3'd4 && cmd != 3'd4) -> (tccd_base >= 24); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.ddr5_timing"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d\n", it.cmd_prev, it.cmd, it.ap_prev, it.cancel_prev, it.max_tccd, it.tccd, it.tccd_base);
  endfunction
endclass
