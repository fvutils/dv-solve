// hard.sudoku_4x4: 4x4 sudoku: rows/cols/boxes all-different, 72 !=
class sb_item;
  rand bit [2:0] c00;
  rand bit [2:0] c01;
  rand bit [2:0] c02;
  rand bit [2:0] c03;
  rand bit [2:0] c10;
  rand bit [2:0] c11;
  rand bit [2:0] c12;
  rand bit [2:0] c13;
  rand bit [2:0] c20;
  rand bit [2:0] c21;
  rand bit [2:0] c22;
  rand bit [2:0] c23;
  rand bit [2:0] c30;
  rand bit [2:0] c31;
  rand bit [2:0] c32;
  rand bit [2:0] c33;
  function bit ok();
    if (!(c00 inside {[1:4]})) return 0;
    if (!(c01 inside {[1:4]})) return 0;
    if (!(c02 inside {[1:4]})) return 0;
    if (!(c03 inside {[1:4]})) return 0;
    if (!(c10 inside {[1:4]})) return 0;
    if (!(c11 inside {[1:4]})) return 0;
    if (!(c12 inside {[1:4]})) return 0;
    if (!(c13 inside {[1:4]})) return 0;
    if (!(c20 inside {[1:4]})) return 0;
    if (!(c21 inside {[1:4]})) return 0;
    if (!(c22 inside {[1:4]})) return 0;
    if (!(c23 inside {[1:4]})) return 0;
    if (!(c30 inside {[1:4]})) return 0;
    if (!(c31 inside {[1:4]})) return 0;
    if (!(c32 inside {[1:4]})) return 0;
    if (!(c33 inside {[1:4]})) return 0;
    if (!(c00 != c01)) return 0;
    if (!(c00 != c02)) return 0;
    if (!(c00 != c03)) return 0;
    if (!(c01 != c02)) return 0;
    if (!(c01 != c03)) return 0;
    if (!(c02 != c03)) return 0;
    if (!(c10 != c11)) return 0;
    if (!(c10 != c12)) return 0;
    if (!(c10 != c13)) return 0;
    if (!(c11 != c12)) return 0;
    if (!(c11 != c13)) return 0;
    if (!(c12 != c13)) return 0;
    if (!(c20 != c21)) return 0;
    if (!(c20 != c22)) return 0;
    if (!(c20 != c23)) return 0;
    if (!(c21 != c22)) return 0;
    if (!(c21 != c23)) return 0;
    if (!(c22 != c23)) return 0;
    if (!(c30 != c31)) return 0;
    if (!(c30 != c32)) return 0;
    if (!(c30 != c33)) return 0;
    if (!(c31 != c32)) return 0;
    if (!(c31 != c33)) return 0;
    if (!(c32 != c33)) return 0;
    if (!(c00 != c10)) return 0;
    if (!(c00 != c20)) return 0;
    if (!(c00 != c30)) return 0;
    if (!(c10 != c20)) return 0;
    if (!(c10 != c30)) return 0;
    if (!(c20 != c30)) return 0;
    if (!(c01 != c11)) return 0;
    if (!(c01 != c21)) return 0;
    if (!(c01 != c31)) return 0;
    if (!(c11 != c21)) return 0;
    if (!(c11 != c31)) return 0;
    if (!(c21 != c31)) return 0;
    if (!(c02 != c12)) return 0;
    if (!(c02 != c22)) return 0;
    if (!(c02 != c32)) return 0;
    if (!(c12 != c22)) return 0;
    if (!(c12 != c32)) return 0;
    if (!(c22 != c32)) return 0;
    if (!(c03 != c13)) return 0;
    if (!(c03 != c23)) return 0;
    if (!(c03 != c33)) return 0;
    if (!(c13 != c23)) return 0;
    if (!(c13 != c33)) return 0;
    if (!(c23 != c33)) return 0;
    if (!(c00 != c01)) return 0;
    if (!(c00 != c10)) return 0;
    if (!(c00 != c11)) return 0;
    if (!(c01 != c10)) return 0;
    if (!(c01 != c11)) return 0;
    if (!(c10 != c11)) return 0;
    if (!(c02 != c03)) return 0;
    if (!(c02 != c12)) return 0;
    if (!(c02 != c13)) return 0;
    if (!(c03 != c12)) return 0;
    if (!(c03 != c13)) return 0;
    if (!(c12 != c13)) return 0;
    if (!(c20 != c21)) return 0;
    if (!(c20 != c30)) return 0;
    if (!(c20 != c31)) return 0;
    if (!(c21 != c30)) return 0;
    if (!(c21 != c31)) return 0;
    if (!(c30 != c31)) return 0;
    if (!(c22 != c23)) return 0;
    if (!(c22 != c32)) return 0;
    if (!(c22 != c33)) return 0;
    if (!(c23 != c32)) return 0;
    if (!(c23 != c33)) return 0;
    if (!(c32 != c33)) return 0;
    return 1;
  endfunction
  constraint c_dom { c00 inside {[1:4]}; c01 inside {[1:4]}; c02 inside {[1:4]}; c03 inside {[1:4]}; c10 inside {[1:4]}; c11 inside {[1:4]}; c12 inside {[1:4]}; c13 inside {[1:4]}; c20 inside {[1:4]}; c21 inside {[1:4]}; c22 inside {[1:4]}; c23 inside {[1:4]}; c30 inside {[1:4]}; c31 inside {[1:4]}; c32 inside {[1:4]}; c33 inside {[1:4]}; }
  constraint c_r0 { c00 != c01; }
  constraint c_r1 { c00 != c02; }
  constraint c_r2 { c00 != c03; }
  constraint c_r3 { c01 != c02; }
  constraint c_r4 { c01 != c03; }
  constraint c_r5 { c02 != c03; }
  constraint c_r6 { c10 != c11; }
  constraint c_r7 { c10 != c12; }
  constraint c_r8 { c10 != c13; }
  constraint c_r9 { c11 != c12; }
  constraint c_r10 { c11 != c13; }
  constraint c_r11 { c12 != c13; }
  constraint c_r12 { c20 != c21; }
  constraint c_r13 { c20 != c22; }
  constraint c_r14 { c20 != c23; }
  constraint c_r15 { c21 != c22; }
  constraint c_r16 { c21 != c23; }
  constraint c_r17 { c22 != c23; }
  constraint c_r18 { c30 != c31; }
  constraint c_r19 { c30 != c32; }
  constraint c_r20 { c30 != c33; }
  constraint c_r21 { c31 != c32; }
  constraint c_r22 { c31 != c33; }
  constraint c_r23 { c32 != c33; }
  constraint c_r24 { c00 != c10; }
  constraint c_r25 { c00 != c20; }
  constraint c_r26 { c00 != c30; }
  constraint c_r27 { c10 != c20; }
  constraint c_r28 { c10 != c30; }
  constraint c_r29 { c20 != c30; }
  constraint c_r30 { c01 != c11; }
  constraint c_r31 { c01 != c21; }
  constraint c_r32 { c01 != c31; }
  constraint c_r33 { c11 != c21; }
  constraint c_r34 { c11 != c31; }
  constraint c_r35 { c21 != c31; }
  constraint c_r36 { c02 != c12; }
  constraint c_r37 { c02 != c22; }
  constraint c_r38 { c02 != c32; }
  constraint c_r39 { c12 != c22; }
  constraint c_r40 { c12 != c32; }
  constraint c_r41 { c22 != c32; }
  constraint c_r42 { c03 != c13; }
  constraint c_r43 { c03 != c23; }
  constraint c_r44 { c03 != c33; }
  constraint c_r45 { c13 != c23; }
  constraint c_r46 { c13 != c33; }
  constraint c_r47 { c23 != c33; }
  constraint c_r48 { c00 != c01; }
  constraint c_r49 { c00 != c10; }
  constraint c_r50 { c00 != c11; }
  constraint c_r51 { c01 != c10; }
  constraint c_r52 { c01 != c11; }
  constraint c_r53 { c10 != c11; }
  constraint c_r54 { c02 != c03; }
  constraint c_r55 { c02 != c12; }
  constraint c_r56 { c02 != c13; }
  constraint c_r57 { c03 != c12; }
  constraint c_r58 { c03 != c13; }
  constraint c_r59 { c12 != c13; }
  constraint c_r60 { c20 != c21; }
  constraint c_r61 { c20 != c30; }
  constraint c_r62 { c20 != c31; }
  constraint c_r63 { c21 != c30; }
  constraint c_r64 { c21 != c31; }
  constraint c_r65 { c30 != c31; }
  constraint c_r66 { c22 != c23; }
  constraint c_r67 { c22 != c32; }
  constraint c_r68 { c22 != c33; }
  constraint c_r69 { c23 != c32; }
  constraint c_r70 { c23 != c33; }
  constraint c_r71 { c32 != c33; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("hard.sudoku_4x4"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d\n", it.c00, it.c01, it.c02, it.c03, it.c10, it.c11, it.c12, it.c13, it.c20, it.c21, it.c22, it.c23, it.c30, it.c31, it.c32, it.c33);
  endfunction
endclass
