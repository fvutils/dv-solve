// scenario.ddr5_cmd_basic: DDR5 command fields by command type (D test_ddr5_cmd_basic)
class sb_item;
  rand bit [2:0] cmd;
  rand bit [1:0] ba;
  rand bit [2:0] bg;
  rand bit [3:0] cid;
  rand bit [7:0] row_hi;
  rand bit [9:0] row_lo;
  rand bit [8:0] col;
  rand bit ap;
  function bit ok();
    if (!(cmd <= 3'd4)) return 0;
    if ((cmd == 3'd0) && !(row_hi + row_lo > 0)) return 0;
    if ((cmd == 3'd1) && !(row_hi > 0)) return 0;
    if ((cmd != 3'd1) && !(col == 0)) return 0;
    if ((cmd != 3'd1) && !(ap == 0)) return 0;
    return 1;
  endfunction
  constraint c_dom { cmd <= 3'd4; }
  constraint c_act_row { (cmd == 3'd0) -> (row_hi + row_lo > 0); }
  constraint c_rd_row { (cmd == 3'd1) -> (row_hi > 0); }
  constraint c_rd_col { (cmd != 3'd1) -> (col == 0); }
  constraint c_rd_ap { (cmd != 3'd1) -> (ap == 0); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.ddr5_cmd_basic"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d\n", it.cmd, it.ba, it.bg, it.cid, it.row_hi, it.row_lo, it.col, it.ap);
  endfunction
endclass
