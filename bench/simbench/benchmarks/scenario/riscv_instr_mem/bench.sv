// scenario.riscv_instr_mem: RV32I load/store: sign-extended offset, aligned EA inside data region
class sb_item;
  rand bit [2:0] funct3;
  rand bit is_store;
  rand bit [4:0] rs1;
  rand bit [4:0] rd;
  rand bit [11:0] imm;
  rand bit [31:0] base;
  rand bit [31:0] ea;
  function bit ok();
    if (!(rs1 != 0)) return 0;
    if (!(rd != 0)) return 0;
    if (!(rd != rs1)) return 0;
    if (!(base inside {[32'h8000_0000:32'h8000_0FFF]})) return 0;
    if ((is_store) && !(funct3 <= 3'd2)) return 0;
    if ((!is_store) && !(funct3 inside {3'd0,3'd1,3'd2,3'd4,3'd5})) return 0;
    if (!(ea == base + {{20{imm[11]}}, imm})) return 0;
    if (!(ea inside {[32'h8000_0000:32'h8000_0FFF]})) return 0;
    if ((funct3[1:0] == 2'd1) && !(ea[0] == 0)) return 0;
    if ((funct3[1:0] == 2'd2) && !(ea[1:0] == 0)) return 0;
    return 1;
  endfunction
  constraint c_dom { rs1 != 0; rd != 0; rd != rs1; base inside {[32'h8000_0000:32'h8000_0FFF]}; }
  constraint c_st { (is_store) -> (funct3 <= 3'd2); }
  constraint c_ld { (!is_store) -> (funct3 inside {3'd0,3'd1,3'd2,3'd4,3'd5}); }
  constraint c_ea { ea == base + {{20{imm[11]}}, imm}; }
  constraint c_win { ea inside {[32'h8000_0000:32'h8000_0FFF]}; }
  constraint c_ah { (funct3[1:0] == 2'd1) -> (ea[0] == 0); }
  constraint c_aw { (funct3[1:0] == 2'd2) -> (ea[1:0] == 0); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.riscv_instr_mem"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d\n", it.funct3, it.is_store, it.rs1, it.rd, it.imm, it.base, it.ea);
  endfunction
endclass
