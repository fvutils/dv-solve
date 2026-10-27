// scenario.i2c_xfer: I2C transfer: reserved 7-bit addresses, page wrap limit
class sb_item;
  rand bit [9:0] addr;
  rand bit ten_bit;
  rand bit rnw;
  rand bit [7:0] nbytes;
  rand bit [7:0] reg_off;
  rand bit repeated_start;
  function bit ok();
    if (!(nbytes inside {[1:32]})) return 0;
    if ((!ten_bit) && !(addr < 10'd128 && !(addr inside {[0:7]}) && !(addr inside {[120:127]}))) return 0;
    if ((ten_bit) && !(addr >= 10'd128)) return 0;
    if ((rnw) && !(repeated_start)) return 0;
    if (!({1'b0, reg_off} + nbytes <= 9'd256)) return 0;
    return 1;
  endfunction
  constraint c_dom { nbytes inside {[1:32]}; }
  constraint c_7b { (!ten_bit) -> (addr < 10'd128 && !(addr inside {[0:7]}) && !(addr inside {[120:127]})); }
  constraint c_10b { (ten_bit) -> (addr >= 10'd128); }
  constraint c_rs { (rnw) -> (repeated_start); }
  constraint c_page { {1'b0, reg_off} + nbytes <= 9'd256; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.i2c_xfer"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d\n", it.addr, it.ten_bit, it.rnw, it.nbytes, it.reg_off, it.repeated_start);
  endfunction
endclass
