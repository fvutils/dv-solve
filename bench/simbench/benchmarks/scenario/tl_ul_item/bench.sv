// scenario.tl_ul_item: TL-UL A-channel: size/address/mask consistency (after OpenTitan tl_seq_item)
class sb_item;
  rand bit [2:0] a_opcode;
  rand bit [1:0] a_size;
  rand bit [31:0] a_address;
  rand bit [3:0] a_mask;
  rand bit [7:0] a_source;
  rand bit [31:0] a_data;
  function bit ok();
    if (!(a_opcode inside {3'd0,3'd1,3'd4})) return 0;
    if (!(a_size <= 2'd2)) return 0;
    if (!(a_source < 8'd64)) return 0;
    if ((a_size == 2'd1) && !(a_address[0] == 0)) return 0;
    if ((a_size == 2'd2) && !(a_address[1:0] == 0)) return 0;
    if ((a_size == 2'd0) && !($countones(a_mask) == 1 && a_mask == (4'b1 << a_address[1:0]))) return 0;
    if ((a_size == 2'd1) && !(a_mask == (a_address[1] ? 4'b1100 : 4'b0011))) return 0;
    if ((a_size == 2'd2) && !(a_mask == 4'hF)) return 0;
    if ((a_opcode == 3'd4) && !(a_data == 0)) return 0;
    if ((a_opcode == 3'd0) && !(a_size == 2'd2)) return 0;
    return 1;
  endfunction
  constraint c_dom { a_opcode inside {3'd0,3'd1,3'd4}; a_size <= 2'd2; a_source < 8'd64; }
  constraint c_align_h { (a_size == 2'd1) -> (a_address[0] == 0); }
  constraint c_align_w { (a_size == 2'd2) -> (a_address[1:0] == 0); }
  constraint c_mask_b { (a_size == 2'd0) -> ($countones(a_mask) == 1 && a_mask == (4'b1 << a_address[1:0])); }
  constraint c_mask_h { (a_size == 2'd1) -> (a_mask == (a_address[1] ? 4'b1100 : 4'b0011)); }
  constraint c_mask_w { (a_size == 2'd2) -> (a_mask == 4'hF); }
  constraint c_get { (a_opcode == 3'd4) -> (a_data == 0); }
  constraint c_full { (a_opcode == 3'd0) -> (a_size == 2'd2); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.tl_ul_item"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d\n", it.a_opcode, it.a_size, it.a_address, it.a_mask, it.a_source, it.a_data);
  endfunction
endclass
