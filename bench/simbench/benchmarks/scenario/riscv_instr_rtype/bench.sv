// scenario.riscv_instr_rtype: RV32IM R-type encoding with reserved-register avoidance (riscv-dv style)
class sb_item;
  rand bit [6:0] opcode;
  rand bit [4:0] rd;
  rand bit [2:0] funct3;
  rand bit [4:0] rs1;
  rand bit [4:0] rs2;
  rand bit [6:0] funct7;
  function bit ok();
    if (!(opcode == 7'b0110011)) return 0;
    if (!(rd != 0)) return 0;
    if (!(funct7 inside {7'h00,7'h20,7'h01})) return 0;
    if ((funct7 == 7'h20) && !(funct3 inside {3'd0,3'd5})) return 0;
    if ((funct7 == 7'h01) && !(rs1 != rs2 && rd != rs1)) return 0;
    if (!(!(rs1 inside {5'd2,5'd3,5'd4}))) return 0;
    return 1;
  endfunction
  constraint c_dom { opcode == 7'b0110011; rd != 0; funct7 inside {7'h00,7'h20,7'h01}; }
  constraint c_sub { (funct7 == 7'h20) -> (funct3 inside {3'd0,3'd5}); }
  constraint c_m { (funct7 == 7'h01) -> (rs1 != rs2 && rd != rs1); }
  constraint c_avoid { !(rs1 inside {5'd2,5'd3,5'd4}); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.riscv_instr_rtype"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d\n", it.opcode, it.rd, it.funct3, it.rs1, it.rs2, it.funct7);
  endfunction
endclass
