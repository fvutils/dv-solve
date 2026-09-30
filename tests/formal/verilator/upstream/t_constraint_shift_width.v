// DESCRIPTION: Verilator: randomize() with shifts on non-32-bit operands
//
// In 5.046, Verilator emits every shift (<<, >>, >>>) in a constraint with a
// 32-bit shift amount, whatever the width of the shifted operand. SMT-LIB2
// requires both operands of bvshl/bvlshr/bvashr to have the same width, so
// for any operand that is not exactly 32 bits the solver rejects the
// constraint. With z3 (the default VERILATOR_SOLVER):
//
//   %Warning: verilated_random.cpp:624: Internal: Solver error: (error
//     "line 9 column 45: Argument #x00000002 at position 1 has sort
//     (_ BitVec 32) it does not match declaration
//     (declare-fun bvlshr ((_ BitVec 8) (_ BitVec 8)) (_ BitVec 8))")
//
// and randomize() returns 0. Emitted for c_lshr8 below:
//
//   (declare-fun x8 () (_ BitVec 8))
//   (assert (= #b1 (__Vbv (= (bvlshr x8 #x00000002) #x03))))
//                                        ^^^^^^^^^ should be #x02
//
// Expected: the shift amount is zero-extended or truncated to the width of
// the shifted operand (for a variable amount, with the out-of-range case
// giving the SystemVerilog result: 0 for << and >>, the sign fill for >>>).
//
// Observed with Verilator 5.046 + z3 4.16.0: every class below except
// Lshr32 fails. Each class holds one shift so a failure names its case.
//
// This file ONLY is a public domain test; the author is Matthew Ballance.
// SPDX-License-Identifier: CC0-1.0

`define checkd(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0d exp=%0d\n", `__FILE__,`__LINE__, (gotv), (expv)); $stop; end while(0);

class Shl8;    rand bit [7:0]         x8;  constraint c_shl8   { (x8 << 2) == 8'h0c; }    endclass
class Lshr8;   rand bit [7:0]         x8;  constraint c_lshr8  { (x8 >> 2) == 8'h03; }    endclass
class Ashr8;   rand bit signed [7:0]  x8;  constraint c_ashr8  { (x8 >>> 2) == -8'sd3; }  endclass
class Lshr16;  rand bit [15:0]        x16; constraint c_lshr16 { (x16 >> 2) == 16'h3; }   endclass
class Lshr32;  rand bit [31:0]        x32; constraint c_lshr32 { (x32 >> 2) == 32'h3; }   endclass
class Lshr64;  rand bit [63:0]        x64; constraint c_lshr64 { (x64 >> 2) == 64'h3; }   endclass
class VarShr8;
   rand bit [7:0] x8;
   rand bit [2:0] s;
   constraint c_var { (x8 >> s) == 8'h03; }
endclass

module t;
   Shl8    shl8    = new;
   Lshr8   lshr8   = new;
   Ashr8   ashr8   = new;
   Lshr16  lshr16  = new;
   Lshr32  lshr32  = new;
   Lshr64  lshr64  = new;
   VarShr8 varshr8 = new;

   initial begin
      // Each call must succeed and its result must satisfy the constraint.
      `checkd(shl8.randomize(), 1);    `checkd(shl8.x8 << 2, 8'h0c);
      `checkd(lshr8.randomize(), 1);   `checkd(lshr8.x8 >> 2, 8'h03);
      `checkd(ashr8.randomize(), 1);   `checkd(ashr8.x8 >>> 2, -8'sd3);
      `checkd(lshr16.randomize(), 1);  `checkd(lshr16.x16 >> 2, 16'h3);
      `checkd(lshr32.randomize(), 1);  `checkd(lshr32.x32 >> 2, 32'h3);
      `checkd(lshr64.randomize(), 1);  `checkd(lshr64.x64 >> 2, 64'h3);
      `checkd(varshr8.randomize(), 1); `checkd(varshr8.x8 >> varshr8.s, 8'h03);

      $write("*-* All Finished *-*\n");
      $finish;
   end
endmodule
