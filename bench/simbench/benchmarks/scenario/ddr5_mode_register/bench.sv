// scenario.ddr5_mode_register: DDR5 mode-register opcode fields (D test_ddr5_mode_register)
class sb_item;
  rand bit [1:0] mra;
  rand bit [1:0] op_burst_len;
  rand bit [4:0] op_cas_lat;
  rand bit op_rfu_7;
  rand bit [2:0] op_rd_pre;
  rand bit op_rfu_5;
  rand bit op_crc_en;
  function bit ok();
    if (!(mra <= 2'd2)) return 0;
    if ((mra == 0) && !(op_burst_len <= 2'd2)) return 0;
    if ((mra == 0) && !(op_cas_lat <= 5'd22)) return 0;
    if ((mra == 0) && !(op_rfu_7 == 0)) return 0;
    if ((mra == 1) && !(op_rd_pre <= 3'd4)) return 0;
    if ((mra == 1) && !(op_rfu_5 == 0)) return 0;
    if ((mra != 0) && !(op_burst_len == 0 && op_cas_lat == 0 && op_rfu_7 == 0)) return 0;
    if ((mra != 1) && !(op_rd_pre == 0 && op_rfu_5 == 0)) return 0;
    if ((mra != 2) && !(op_crc_en == 0)) return 0;
    return 1;
  endfunction
  constraint c_dom { mra <= 2'd2; }
  constraint c_mr0_burst { (mra == 0) -> (op_burst_len <= 2'd2); }
  constraint c_mr0_cas { (mra == 0) -> (op_cas_lat <= 5'd22); }
  constraint c_mr0_rfu { (mra == 0) -> (op_rfu_7 == 0); }
  constraint c_mr8_pre { (mra == 1) -> (op_rd_pre <= 3'd4); }
  constraint c_mr8_rfu { (mra == 1) -> (op_rfu_5 == 0); }
  constraint c_zero_mr0 { (mra != 0) -> (op_burst_len == 0 && op_cas_lat == 0 && op_rfu_7 == 0); }
  constraint c_zero_mr8 { (mra != 1) -> (op_rd_pre == 0 && op_rfu_5 == 0); }
  constraint c_zero_mr50 { (mra != 2) -> (op_crc_en == 0); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.ddr5_mode_register"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d\n", it.mra, it.op_burst_len, it.op_cas_lat, it.op_rfu_7, it.op_rd_pre, it.op_rfu_5, it.op_crc_en);
  endfunction
endclass
