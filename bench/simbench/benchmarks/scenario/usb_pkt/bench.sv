// scenario.usb_pkt: USB full-speed packet: PID, transfer-type payload limits
class sb_item;
  rand bit [3:0] pid;
  rand bit [6:0] dev_addr;
  rand bit [3:0] endp;
  rand bit [10:0] len;
  rand bit [1:0] xfer;
  function bit ok();
    if (!(pid inside {4'b0001,4'b1001,4'b0011,4'b1011,4'b0010})) return 0;
    if ((pid inside {4'b0001,4'b1001}) && !(len == 0)) return 0;
    if ((xfer == 2'd0) && !(endp == 0 && len <= 11'd64)) return 0;
    if ((xfer == 2'd1) && !(len <= 11'd1023)) return 0;
    if ((xfer == 2'd2) && !(len inside {11'd0,11'd8,11'd16,11'd32,11'd64})) return 0;
    if ((xfer == 2'd3) && !(len <= 11'd64 && endp != 0)) return 0;
    if (!(dev_addr != 0)) return 0;
    if ((pid == 4'b0010) && !(len == 0)) return 0;
    return 1;
  endfunction
  constraint c_dom { pid inside {4'b0001,4'b1001,4'b0011,4'b1011,4'b0010}; }
  constraint c_tok { (pid inside {4'b0001,4'b1001}) -> (len == 0); }
  constraint c_ctl { (xfer == 2'd0) -> (endp == 0 && len <= 11'd64); }
  constraint c_iso { (xfer == 2'd1) -> (len <= 11'd1023); }
  constraint c_bulk { (xfer == 2'd2) -> (len inside {11'd0,11'd8,11'd16,11'd32,11'd64}); }
  constraint c_int { (xfer == 2'd3) -> (len <= 11'd64 && endp != 0); }
  constraint c_addr { dev_addr != 0; }
  constraint c_ack { (pid == 4'b0010) -> (len == 0); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.usb_pkt"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d\n", it.pid, it.dev_addr, it.endp, it.len, it.xfer);
  endfunction
endclass
