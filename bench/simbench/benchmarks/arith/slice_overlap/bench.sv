// arith.slice_overlap: overlapping part-selects of one word, the shape of
// riscv-dv's riscv_illegal_instr (see scenario.riscv_illegal_instr).
// w[15:13] == hi3 always; by `kind`, a wider select over the same bits is
// fixed (w[15:5] == 0, w[15:10] == 6'b100111, w[15:0] == 0), which pins hi3.
// hi3 is solved before w, so a solver that decides hi3 first and refutes a
// wrong value only through w's interval bounds walks the 2^16 settings of the
// free w[31:16]. !(w[31:20] inside {excl}) adds one select of w per element
// of a state queue (64 here).
class sb_item;
  rand bit [31:0] w;
  rand bit [2:0]  hi3;
  rand bit [1:0]  lo2;
  rand bit [1:0]  kind;
  rand bit        comp;
  rand bit [6:0]  op7;
  bit [11:0] excl[$];
  function new();
    for (int i = 0; i < 64; i++) excl.push_back(12'h300 + 12'(i));
  endfunction
  constraint c_order { solve comp before kind; solve kind before hi3; solve hi3 before w; solve lo2 before w; }
  constraint c_sel {
    lo2 != 2'b11;
    if (comp) {
      w[15:13] == hi3;
      w[1:0] == lo2;
      (kind == 0) -> (w[15:5] == '0 && lo2 == 2'b00);
      (kind == 1) -> (w[15:10] == 6'b100111 && lo2 == 2'b01);
      (kind == 2) -> (w[15:0] == 16'h0);
    } else {
      w[6:0] == op7;
      op7[1:0] == 2'b11;
      !(w[31:20] inside {excl});
    }
  }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("arith.slice_overlap"); endfunction
  virtual function bit do_randomize(); return it.randomize() with { kind != 3; }; endfunction
  virtual function int check();
    int e = 0;
    if (it.kind == 3 || it.lo2 == 3) e++;
    if (it.comp) begin
      if (it.w[15:13] != it.hi3 || it.w[1:0] != it.lo2) e++;
      if (it.kind == 0 && !(it.w[15:5] == 0 && it.lo2 == 0)) e++;
      if (it.kind == 1 && !(it.w[15:10] == 6'b100111 && it.lo2 == 1)) e++;
      if (it.kind == 2 && it.w[15:0] != 0) e++;
    end else begin
      if (it.w[6:0] != it.op7 || it.op7[1:0] != 2'b11) e++;
      if (it.w[31:20] inside {it.excl}) e++;
    end
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d\n", it.comp, it.kind, it.hi3, it.lo2, it.w);
  endfunction
endclass
