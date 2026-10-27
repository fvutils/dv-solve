// scenario.ddr5_timing_full: DDR5 spacing with cancel and read latency (D test_ddr5_timing_full)
class sb_item;
  rand bit [2:0] cmd_prev;
  rand bit [2:0] cmd;
  rand bit cancel_prev;
  rand bit [5:0] rl;
  rand bit [3:0] max_slack;
  rand bit [5:0] tccd_base;
  rand bit [5:0] tccd;
  rand bit valid;
  function bit ok();
    if (!(cmd_prev <= 3'd4)) return 0;
    if (!(cmd <= 3'd4)) return 0;
    if (!(rl inside {[22:40]})) return 0;
    if (!(max_slack <= 4'd8)) return 0;
    if (!(tccd_base inside {[2:50]})) return 0;
    if (!(tccd inside {[2:58]})) return 0;
    if (!(valid == 1)) return 0;
    if (!(tccd == tccd_base + max_slack)) return 0;
    if ((cancel_prev == 1) && !(tccd_base >= 8)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd2 && (cmd == 3'd2)) && !(tccd_base >= 8)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd2 && (cmd != 3'd2)) && !(tccd_base >= 16)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd3 && (cmd == 3'd3)) && !(tccd_base >= 16)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd3 && (cmd == 3'd2)) && !(tccd_base >= rl + 9)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd3 && (cmd != 3'd3 && cmd != 3'd2)) && !(tccd_base >= 16)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd0 && (cmd == 3'd0)) && !(tccd_base >= 8)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd0 && (cmd != 3'd0)) && !(tccd_base >= 24)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd1 && (cmd == 3'd1)) && !(tccd_base >= 8)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd1 && (cmd == 3'd4)) && !(tccd_base >= 12)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd1 && (cmd != 3'd1 && cmd != 3'd4)) && !(tccd_base >= 24)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd4 && (cmd == 3'd4)) && !(tccd_base >= 2)) return 0;
    if ((cancel_prev == 0 && cmd_prev == 3'd4 && (cmd != 3'd4)) && !(tccd_base >= 24)) return 0;
    return 1;
  endfunction
  constraint c_dom { cmd_prev <= 3'd4; cmd <= 3'd4; rl inside {[22:40]}; max_slack <= 4'd8; tccd_base inside {[2:50]}; tccd inside {[2:58]}; valid == 1; }
  constraint c_tccd_sum { tccd == tccd_base + max_slack; }
  constraint c_cancel { (cancel_prev == 1) -> (tccd_base >= 8); }
  constraint c_mrw_mrw { (cancel_prev == 0 && cmd_prev == 3'd2 && (cmd == 3'd2)) -> (tccd_base >= 8); }
  constraint c_mrw_other { (cancel_prev == 0 && cmd_prev == 3'd2 && (cmd != 3'd2)) -> (tccd_base >= 16); }
  constraint c_mrr_mrr { (cancel_prev == 0 && cmd_prev == 3'd3 && (cmd == 3'd3)) -> (tccd_base >= 16); }
  constraint c_mrr_mrw { (cancel_prev == 0 && cmd_prev == 3'd3 && (cmd == 3'd2)) -> (tccd_base >= rl + 9); }
  constraint c_mrr_other { (cancel_prev == 0 && cmd_prev == 3'd3 && (cmd != 3'd3 && cmd != 3'd2)) -> (tccd_base >= 16); }
  constraint c_act_act { (cancel_prev == 0 && cmd_prev == 3'd0 && (cmd == 3'd0)) -> (tccd_base >= 8); }
  constraint c_act_other { (cancel_prev == 0 && cmd_prev == 3'd0 && (cmd != 3'd0)) -> (tccd_base >= 24); }
  constraint c_rd_rd { (cancel_prev == 0 && cmd_prev == 3'd1 && (cmd == 3'd1)) -> (tccd_base >= 8); }
  constraint c_rd_pre { (cancel_prev == 0 && cmd_prev == 3'd1 && (cmd == 3'd4)) -> (tccd_base >= 12); }
  constraint c_rd_other { (cancel_prev == 0 && cmd_prev == 3'd1 && (cmd != 3'd1 && cmd != 3'd4)) -> (tccd_base >= 24); }
  constraint c_pre_pre { (cancel_prev == 0 && cmd_prev == 3'd4 && (cmd == 3'd4)) -> (tccd_base >= 2); }
  constraint c_pre_other { (cancel_prev == 0 && cmd_prev == 3'd4 && (cmd != 3'd4)) -> (tccd_base >= 24); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.ddr5_timing_full"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d\n", it.cmd_prev, it.cmd, it.cancel_prev, it.rl, it.max_slack, it.tccd_base, it.tccd, it.valid);
  endfunction
endclass
