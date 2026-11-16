// scenario.riscv_illegal_instr: riscv-dv's riscv_illegal_instr (rv32imc), called as
// insert_illegal_hint_instr calls it: randomize() with {exception != kHintInstr;}.
// One 32-bit word (instr_bin) is tied to other rand fields through many
// overlapping part-selects ([1:0], [6:0], [11:7], [12], [14:12], [15:13],
// [15:10], [15:0], [31:20], ...) under implications and solve-before, and
// !(instr_bin[31:20] inside {csrs}) excludes 327 CSR addresses (a state queue).
// Per call, dv-solve --solver-dvs refutes inconsistent part-select values by a
// bounds walk over instr_bin (docs/design/dvs_hotspot_h2.md in verilator-dvs-integ).
class sb_item;
  typedef enum bit [2:0] {
    kIllegalOpcode, kIllegalCompressedOpcode, kIllegalFunc3, kIllegalFunc7,
    kReservedCompressedInstr, kHintInstr, kIllegalSystemInstr
  } illegal_instr_type_e;
  typedef enum bit [3:0] {
    kIllegalCompressed, kReservedAddispn, kReservedAddiw, kReservedAddi16sp,
    kReservedLui, kReservedLwsp, kReservedLdsp, kReservedLqsp, kReservedJr,
    kReservedC0, kReservedC1, kReservedC2
  } reserved_c_instr_e;
  typedef enum {RV32I, RV64I, RV32M, RV64M, RV32A, RV64A, RV32F, RV32FC, RV64F, RV32D,
                RV32DC, RV64D, RV32C, RV64C, RV128I, RV128C, RV32B, RV32ZBA, RV32ZBB,
                RV32ZBC, RV32ZBS, RV32ZCB} riscv_instr_group_t;
  localparam int XLEN = 32;
  // rv32imc; riscv-dv reads it from the core setting (a state queue).
  riscv_instr_group_t supported_isa[$] = '{RV32I, RV32M, RV32C};

  bit [6:0] legal_opcode[$] = '{7'b0000011, 7'b0001111, 7'b0010011, 7'b0010111,
                                 7'b0100011, 7'b0110111, 7'b1100011, 7'b0110011,
                                 7'b1100111, 7'b1110011, 7'b1101111};
  bit [2:0] legal_c00_opcode[$] = '{3'b000, 3'b010, 3'b110};
  bit [2:0] legal_c10_opcode[$] = '{3'b000, 3'b010, 3'b100, 3'b110};
  bit [11:0] csrs[$];   // riscv-dv's privileged_reg_t values, filled in new()
  bit [11:0] custom_csr[] = {};

  rand illegal_instr_type_e exception;
  rand reserved_c_instr_e   reserved_c;
  rand bit [31:0]           instr_bin;
  rand bit [6:0]            opcode;
  rand bit                  compressed;
  rand bit [2:0]            func3;
  rand bit [6:0]            func7;
  rand bit                  has_func3;
  rand bit                  has_func7;
  rand bit [1:0]            c_op;
  rand bit [2:0]            c_msb;

  function new();
    bit [11:0] v[$] = '{
      12'h000,12'h004,12'h005,12'h040,12'h041,12'h042,12'h043,12'h044,12'h001,12'h002,12'h003,12'hC00,
      12'hC01,12'hC02,12'hC03,12'hC04,12'hC05,12'hC06,12'hC07,12'hC08,12'hC09,12'hC0A,12'hC0B,12'hC0C,
      12'hC0D,12'hC0E,12'hC0F,12'hC10,12'hC11,12'hC12,12'hC13,12'hC14,12'hC15,12'hC16,12'hC17,12'hC18,
      12'hC19,12'hC1A,12'hC1B,12'hC1C,12'hC1D,12'hC1E,12'hC1F,12'hC80,12'hC81,12'hC82,12'hC83,12'hC84,
      12'hC85,12'hC86,12'hC87,12'hC88,12'hC89,12'hC8A,12'hC8B,12'hC8C,12'hC8D,12'hC8E,12'hC8F,12'hC90,
      12'hC91,12'hC92,12'hC93,12'hC94,12'hC95,12'hC96,12'hC97,12'hC98,12'hC99,12'hC9A,12'hC9B,12'hC9C,
      12'hC9D,12'hC9E,12'hC9F,12'h100,12'h102,12'h103,12'h104,12'h105,12'h106,12'h10A,12'h140,12'h141,
      12'h142,12'h143,12'h144,12'h180,12'h5A8,12'h600,12'h602,12'h603,12'h604,12'h606,12'h607,12'h643,
      12'h644,12'h645,12'h64A,12'hE12,12'h60A,12'h61A,12'h680,12'h6A8,12'h605,12'h615,12'h200,12'h204,
      12'h205,12'h240,12'h241,12'h242,12'h243,12'h244,12'h280,12'hF11,12'hF12,12'hF13,12'hF14,12'hF15,
      12'h300,12'h301,12'h302,12'h303,12'h304,12'h305,12'h306,12'h310,12'h340,12'h341,12'h342,12'h343,
      12'h344,12'h30A,12'h31A,12'h747,12'h757,12'h3A0,12'h3A1,12'h3A2,12'h3A3,12'h3A4,12'h3A5,12'h3A6,
      12'h3A7,12'h3A8,12'h3A9,12'h3AA,12'h3AB,12'h3AC,12'h3AD,12'h3AE,12'h3AF,12'h3B0,12'h3B1,12'h3B2,
      12'h3B3,12'h3B4,12'h3B5,12'h3B6,12'h3B7,12'h3B8,12'h3B9,12'h3BA,12'h3BB,12'h3BC,12'h3BD,12'h3BE,
      12'h3BF,12'h4C0,12'h3C1,12'h3C2,12'h3C3,12'h3C4,12'h3C5,12'h3C6,12'h3C7,12'h3C8,12'h3C9,12'h3CA,
      12'h3CB,12'h3CC,12'h3CD,12'h3CE,12'h3CF,12'h4D0,12'h3D1,12'h3D2,12'h3D3,12'h3D4,12'h3D5,12'h3D6,
      12'h3D7,12'h3D8,12'h3D9,12'h3DA,12'h3DB,12'h3DC,12'h3DD,12'h3DE,12'h3DF,12'h4E0,12'h3E1,12'h3E2,
      12'h3E3,12'h3E4,12'h3E5,12'h3E6,12'h3E7,12'h3E8,12'h3E9,12'h3EA,12'h3EB,12'h3EC,12'h3ED,12'h3EE,
      12'h3EF,12'hB00,12'hB02,12'hB03,12'hB04,12'hB05,12'hB06,12'hB07,12'hB08,12'hB09,12'hB0A,12'hB0B,
      12'hB0C,12'hB0D,12'hB0E,12'hB0F,12'hB10,12'hB11,12'hB12,12'hB13,12'hB14,12'hB15,12'hB16,12'hB17,
      12'hB18,12'hB19,12'hB1A,12'hB1B,12'hB1C,12'hB1D,12'hB1E,12'hB1F,12'hB80,12'hB82,12'hB83,12'hB84,
      12'hB85,12'hB86,12'hB87,12'hB88,12'hB89,12'hB8A,12'hB8B,12'hB8C,12'hB8D,12'hB8E,12'hB8F,12'hB90,
      12'hB91,12'hB92,12'hB93,12'hB94,12'hB95,12'hB96,12'hB97,12'hB98,12'hB99,12'hB9A,12'hB9B,12'hB9C,
      12'hB9D,12'hB9E,12'hB9F,12'h320,12'h323,12'h324,12'h325,12'h326,12'h327,12'h328,12'h329,12'h32A,
      12'h32B,12'h32C,12'h32D,12'h32E,12'h32F,12'h330,12'h331,12'h332,12'h333,12'h334,12'h335,12'h336,
      12'h337,12'h338,12'h339,12'h33A,12'h33B,12'h33C,12'h33D,12'h33E,12'h33F,12'h7A0,12'h7A1,12'h7A2,
      12'h7A3,12'h7A4,12'h7A5,12'h7A8,12'h7AA,12'h7B0,12'h7B1,12'h7B2,12'h7B3,12'h008,12'h009,12'h00A,
      12'hC20,12'hC21,12'hC22};
    csrs = v;
  endfunction

  // riscv-dv's constraints, as written (ISA guards read supported_isa).
  constraint exception_dist_c {
    exception dist {
      kIllegalOpcode := 3, kIllegalCompressedOpcode := 1, kIllegalFunc3 := 1,
      kIllegalFunc7 := 1, kReservedCompressedInstr := 1, kHintInstr := 3,
      kIllegalSystemInstr := 3
    };
    if (!(RV32C inside {supported_isa})) {
      exception != kHintInstr;
      compressed == 1'b0;
    }
  }
  constraint instr_bit_assignment_c {
    solve opcode before instr_bin;
    solve func3 before instr_bin;
    solve func7 before instr_bin;
    solve c_msb before instr_bin;
    solve c_op before instr_bin;
    if (compressed) {
      instr_bin[1:0] == c_op;
      instr_bin[15:13] == c_msb;
    } else {
      instr_bin[6:0] == opcode;
      if (has_func7) instr_bin[31:25] == func7;
      if (has_func3) instr_bin[14:12] == func3;
    }
  }
  constraint system_instr_c {
    if (exception == kIllegalSystemInstr) {
      opcode == 7'b1110011;
      if (func3 == 3'b000) {
        instr_bin[19:15] != 0;
        instr_bin[11:7] != 0;
        !(instr_bin[31:20] inside {12'h0, 12'h1, 12'h2, 12'h102, 12'h302, 12'h7b2, 12'h105});
      } else {
        !(instr_bin[31:20] inside {csrs});
        !(instr_bin[31:20] inside {custom_csr});
      }
    }
  }
  constraint legal_rv32_c_slli {
    if ((c_msb == 3'b000) && (c_op == 2'b10) && (XLEN == 32)) {
      if (exception == kReservedCompressedInstr) instr_bin[12] == 1;
      else instr_bin[12] == 0;
    }
  }
  constraint exception_type_c {
    if (compressed) {
      exception inside {kReservedCompressedInstr, kIllegalCompressedOpcode, kHintInstr};
    } else {
      exception inside {kIllegalOpcode, kIllegalFunc3, kIllegalFunc7, kIllegalSystemInstr};
    }
    if (!has_func7) exception != kIllegalFunc7;
    if (!has_func3) exception != kIllegalFunc3;
  }
  constraint compressed_instr_op_c { c_op != 2'b11; }
  constraint b_extension_c {
    if (RV32B inside {supported_isa}) {
      if (exception inside {kIllegalFunc3, kIllegalFunc7}) {
        !(opcode inside {7'b0110011, 7'b0010011, 7'b0111011});
      }
    }
  }
  constraint zba_extension_c {
    if (RV32ZBA inside {supported_isa}) {
      if (exception inside {kIllegalFunc3, kIllegalFunc7}) {
        !(opcode inside {7'b0110011, 7'b0111011, 7'b0011011});
      }
    }
  }
  constraint zbb_extension_c {
    if (RV32ZBB inside {supported_isa}) {
      if (exception inside {kIllegalFunc3, kIllegalFunc7}) {
        !(opcode inside {7'b0110011, 7'b0010011, 7'b0111011, 7'b0011011});
      }
    }
  }
  constraint zbc_extension_c {
    if (RV32ZBB inside {supported_isa}) {
      if (exception inside {kIllegalFunc3, kIllegalFunc7}) {
        !(opcode inside {7'b0110011});
      }
    }
  }
  constraint zbs_extension_c {
    if (RV32ZBS inside {supported_isa}) {
      if (exception inside {kIllegalFunc3, kIllegalFunc7}) {
        !(opcode inside {7'b0110011, 7'b0010011});
      }
    }
  }
  constraint zcb_extension_c {
    if (RV32ZCB inside {supported_isa}) {
      if (exception inside {kIllegalCompressedOpcode, kReservedCompressedInstr}
          && c_msb == 3'b100) {
        if (c_op == 2'b00) {
          !(instr_bin[12:10] inside {3'b000, 3'b001, 3'b010, 3'b011});
        } else if (c_op == 2'b01 && instr_bin[12:10] == 3'b111) {
         !(instr_bin[6:5] inside {2'b10});
         if (instr_bin[6:5] == 2'b11) {
           if (XLEN == 32) {
             (instr_bin[4:2] inside {3'b100, 3'b110, 3'b111});
           } else {
             (instr_bin[4:2] inside {3'b110, 3'b111});
           }
         }
        }
      }
    }
  }
  constraint illegal_compressed_op_c {
    if (exception == kIllegalCompressedOpcode) {
      c_op != 2'b01;
      if (legal_c00_opcode.size() == 8) c_op != 2'b00;
      else !(c_msb inside {legal_c00_opcode});
      if (legal_c10_opcode.size() == 8) c_op != 2'b10;
      else !(c_msb inside {legal_c10_opcode});
    }
  }
  constraint reserved_compressed_instr_c {
    solve exception  before reserved_c;
    solve exception  before opcode;
    solve reserved_c before instr_bin;
    solve reserved_c before c_msb;
    solve reserved_c before c_op;
    if (XLEN == 32) reserved_c != kReservedAddiw;
    if (exception == kReservedCompressedInstr) {
      (reserved_c == kIllegalCompressed) -> (instr_bin[15:0] == 0);
      (reserved_c == kReservedAddispn)   -> ((instr_bin[15:5] == '0) && (c_op == 2'b00));
      (reserved_c == kReservedAddiw)     -> ((c_msb == 3'b001) && (c_op == 2'b01) &&
                                             (instr_bin[11:7] == 5'b0));
      (reserved_c == kReservedC0)        -> ((instr_bin[15:10] == 6'b100111) &&
                                             (instr_bin[6:5] == 2'b10) && (c_op == 2'b01));
      (reserved_c == kReservedC1)        -> ((instr_bin[15:10] == 6'b100111) &&
                                             (instr_bin[6:5] == 2'b11) && (c_op == 2'b01));
      (reserved_c == kReservedC2)        -> ((c_msb == 3'b100) && (c_op == 2'b00));
      (reserved_c == kReservedAddi16sp)  -> ((c_msb == 3'b011) && (c_op == 2'b01) &&
                                             (instr_bin[11:7] == 2) &&
                                             !instr_bin[12] && (instr_bin[6:2] == 0));
      (reserved_c == kReservedLui)       -> ((c_msb == 3'b011) && (c_op == 2'b01) &&
                                             !instr_bin[12] && (instr_bin[6:2] == 0));
      (reserved_c == kReservedJr)        -> (instr_bin == 16'b1000_0000_0000_0010);
      (reserved_c == kReservedLqsp)      -> ((c_msb == 3'b001) && (c_op == 2'b10) &&
                                             (instr_bin[11:7] == 5'b0));
      (reserved_c == kReservedLwsp)      -> ((c_msb == 3'b010) && (c_op == 2'b10) &&
                                             (instr_bin[11:7] == 5'b0));
      (reserved_c == kReservedLdsp)      -> ((c_msb == 3'b011) && (c_op == 2'b10) &&
                                             (instr_bin[11:7] == 5'b0));
    }
  }
  constraint hint_instr_c {
    if (exception == kHintInstr) {
      ((c_msb == 3'b000) && (c_op == 2'b01) && ({instr_bin[12], instr_bin[6:2]} == 6'b0)) ||
      ((c_msb == 3'b010) && (c_op == 2'b01) && (instr_bin[11:7] == 5'b0)) ||
      ((c_msb == 3'b100) && (c_op == 2'b01) && (instr_bin[12:11] == 2'b00) &&
                                               (instr_bin[6:2] == 5'b0)) ||
      ((c_msb == 3'b100) && (c_op == 2'b10) && (instr_bin[11:7] == 0) &&
                                               (instr_bin[6:2]  != 0)) ||
      ((c_msb == 3'b011) && (c_op == 2'b01) && (instr_bin[11:7] == 5'b0) &&
                                               ({instr_bin[12], instr_bin[6:2]} != 6'b0)) ||
      ((c_msb == 3'b000) && (c_op == 2'b10) && (instr_bin[11:7] == 5'b0))  ||
      ((c_msb == 3'b000) && (c_op == 2'b10) && (instr_bin[11:7] != 5'b0) && !instr_bin[12] &&
                                               (instr_bin[6:2] == 0))  ||
      ((c_msb == 3'b100) && (c_op == 2'b10) && (instr_bin[11:7] == 5'b0) && instr_bin[12] &&
                                               (instr_bin[6:2] != 0));
    }
  }
  constraint illegal_opcode_c {
    solve opcode before instr_bin;
    if (exception == kIllegalOpcode) {
      !(opcode inside {legal_opcode});
      opcode[1:0] == 2'b11;
    } else {
      opcode inside {legal_opcode};
    }
  }
  constraint no_atomic_c {
    if (exception != kIllegalOpcode) opcode != 7'b0101111;
  }
  constraint illegal_func3_c {
    solve opcode before func3;
    if (!compressed) {
      if (exception == kIllegalFunc3) {
        (opcode == 7'b1100111) -> (func3 != 3'b000);
        (opcode == 7'b1100011) -> (func3 inside {3'b010, 3'b011});
        (opcode == 7'b0100011) -> (func3 >= 3'b011);
        (opcode == 7'b0000011) -> (func3 inside {3'b011, 3'b111});
        (opcode == 7'b0001111) -> (!(func3 inside {3'b000, 3'b001}));
        (opcode == 7'b1110011) -> (func3 == 3'b100);
        (opcode == 7'b0011011) -> (!(func3 inside {3'b000, 3'b001, 3'b101}));
        (opcode == 7'b0111011) -> (func3 inside {3'b010, 3'b011});
        opcode inside {7'b1100111, 7'b1100011, 7'b0000011, 7'b0100011,
                       7'b0001111, 7'b1110011, 7'b0011011, 7'b0111011};
      } else {
        (opcode == 7'b1100111) -> (func3 == 3'b000);
        (opcode == 7'b1100011) -> (!(func3 inside {3'b010, 3'b011}));
        (opcode == 7'b0100011) -> (func3 < 3'b011);
        (opcode == 7'b0000011) -> !(func3 inside {3'b011, 3'b111});
        (opcode == 7'b0001111) -> (func3 inside {3'b000, 3'b001});
        (opcode == 7'b1110011) -> (func3 != 3'b100);
        (opcode == 7'b0011011) -> (func3 inside {3'b000, 3'b001, 3'b101});
        (opcode == 7'b0111011) -> (!(func3 inside {3'b010, 3'b011}));
      }
    }
  }
  constraint has_func7_c {
    solve opcode before func7;
    if (((opcode == 7'b0010011) && (func3 inside {3'b001, 3'b101})) ||
        (opcode inside {7'b0110011, 7'b0111011})) {
      has_func7 == 1'b1;
    } else {
      has_func7 == 1'b0;
    }
  }
  constraint has_func3_c {
    solve opcode before func7;
    if ((opcode inside {7'b0110111, 7'b1101111, 7'b0010111})) has_func3 == 1'b0;
    else has_func3 == 1'b1;
  }
  constraint illegal_func7_c {
    if (!compressed) {
      if (exception == kIllegalFunc7) {
        !(func7 inside {7'd0, 7'b0100000, 7'd1});
        if (opcode == 7'b0001001) !(func7[6:1] inside {6'd0, 6'b010000});
      } else {
        func7 inside {7'd0, 7'b0100000, 7'd1};
      }
    }
  }

  // A procedural re-check of the main constraints.
  function int errors();
    int e = 0;
    if (exception == kHintInstr) e++;   // the inline constraint
    if (c_op == 2'b11) e++;
    if (reserved_c == kReservedAddiw) e++;
    if (compressed) begin
      if (!(exception inside {kReservedCompressedInstr, kIllegalCompressedOpcode})) e++;
      if (instr_bin[1:0] != c_op || instr_bin[15:13] != c_msb) e++;
    end else begin
      if (!(exception inside {kIllegalOpcode, kIllegalFunc3, kIllegalFunc7, kIllegalSystemInstr})) e++;
      if (instr_bin[6:0] != opcode) e++;
      if (has_func7 && instr_bin[31:25] != func7) e++;
      if (has_func3 && instr_bin[14:12] != func3) e++;
      if (exception != kIllegalFunc7 && !(func7 inside {7'd0, 7'b0100000, 7'd1})) e++;
    end
    if ((exception == kIllegalOpcode) != !(opcode inside {legal_opcode})) e++;
    if (exception == kIllegalSystemInstr) begin
      if (opcode != 7'b1110011) e++;
      if (func3 != 0 && (instr_bin[31:20] inside {csrs})) e++;
    end
    if (exception == kIllegalCompressedOpcode
        && (c_op == 2'b01 || (c_msb inside {legal_c00_opcode}) || (c_msb inside {legal_c10_opcode}))) e++;
    if (exception == kReservedCompressedInstr) begin
      if (reserved_c == kIllegalCompressed && instr_bin[15:0] != 0) e++;
      if (reserved_c == kReservedC0 && !(instr_bin[15:10] == 6'b100111 && instr_bin[6:5] == 2'b10 && c_op == 2'b01)) e++;
      if (reserved_c == kReservedC2 && !(c_msb == 3'b100 && c_op == 2'b00)) e++;
      if (reserved_c == kReservedJr && instr_bin != 32'h8002) e++;
    end
    return e;
  endfunction
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.riscv_illegal_instr"); endfunction
  virtual function bit do_randomize();
    return it.randomize() with { exception != kHintInstr; };
  endfunction
  virtual function int check(); return it.errors(); endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d %0d\n", it.exception, it.reserved_c, it.compressed,
            it.c_msb, it.c_op, it.opcode, it.func3, it.func7, it.instr_bin);
  endfunction
endclass
