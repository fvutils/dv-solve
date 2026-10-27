// hard.nqueens_8: 8-queens: 84 pairwise != over column/diagonals (D test_nqueens_8)
class sb_item;
  rand bit [3:0] q0;
  rand bit [3:0] q1;
  rand bit [3:0] q2;
  rand bit [3:0] q3;
  rand bit [3:0] q4;
  rand bit [3:0] q5;
  rand bit [3:0] q6;
  rand bit [3:0] q7;
  function bit ok();
    if (!(q0 < 8)) return 0;
    if (!(q1 < 8)) return 0;
    if (!(q2 < 8)) return 0;
    if (!(q3 < 8)) return 0;
    if (!(q4 < 8)) return 0;
    if (!(q5 < 8)) return 0;
    if (!(q6 < 8)) return 0;
    if (!(q7 < 8)) return 0;
    if (!(q0 != q1)) return 0;
    if (!(int'(q0) + 0 != int'(q1) + 1)) return 0;
    if (!(int'(q0) - 0 != int'(q1) - 1)) return 0;
    if (!(q0 != q2)) return 0;
    if (!(int'(q0) + 0 != int'(q2) + 2)) return 0;
    if (!(int'(q0) - 0 != int'(q2) - 2)) return 0;
    if (!(q0 != q3)) return 0;
    if (!(int'(q0) + 0 != int'(q3) + 3)) return 0;
    if (!(int'(q0) - 0 != int'(q3) - 3)) return 0;
    if (!(q0 != q4)) return 0;
    if (!(int'(q0) + 0 != int'(q4) + 4)) return 0;
    if (!(int'(q0) - 0 != int'(q4) - 4)) return 0;
    if (!(q0 != q5)) return 0;
    if (!(int'(q0) + 0 != int'(q5) + 5)) return 0;
    if (!(int'(q0) - 0 != int'(q5) - 5)) return 0;
    if (!(q0 != q6)) return 0;
    if (!(int'(q0) + 0 != int'(q6) + 6)) return 0;
    if (!(int'(q0) - 0 != int'(q6) - 6)) return 0;
    if (!(q0 != q7)) return 0;
    if (!(int'(q0) + 0 != int'(q7) + 7)) return 0;
    if (!(int'(q0) - 0 != int'(q7) - 7)) return 0;
    if (!(q1 != q2)) return 0;
    if (!(int'(q1) + 1 != int'(q2) + 2)) return 0;
    if (!(int'(q1) - 1 != int'(q2) - 2)) return 0;
    if (!(q1 != q3)) return 0;
    if (!(int'(q1) + 1 != int'(q3) + 3)) return 0;
    if (!(int'(q1) - 1 != int'(q3) - 3)) return 0;
    if (!(q1 != q4)) return 0;
    if (!(int'(q1) + 1 != int'(q4) + 4)) return 0;
    if (!(int'(q1) - 1 != int'(q4) - 4)) return 0;
    if (!(q1 != q5)) return 0;
    if (!(int'(q1) + 1 != int'(q5) + 5)) return 0;
    if (!(int'(q1) - 1 != int'(q5) - 5)) return 0;
    if (!(q1 != q6)) return 0;
    if (!(int'(q1) + 1 != int'(q6) + 6)) return 0;
    if (!(int'(q1) - 1 != int'(q6) - 6)) return 0;
    if (!(q1 != q7)) return 0;
    if (!(int'(q1) + 1 != int'(q7) + 7)) return 0;
    if (!(int'(q1) - 1 != int'(q7) - 7)) return 0;
    if (!(q2 != q3)) return 0;
    if (!(int'(q2) + 2 != int'(q3) + 3)) return 0;
    if (!(int'(q2) - 2 != int'(q3) - 3)) return 0;
    if (!(q2 != q4)) return 0;
    if (!(int'(q2) + 2 != int'(q4) + 4)) return 0;
    if (!(int'(q2) - 2 != int'(q4) - 4)) return 0;
    if (!(q2 != q5)) return 0;
    if (!(int'(q2) + 2 != int'(q5) + 5)) return 0;
    if (!(int'(q2) - 2 != int'(q5) - 5)) return 0;
    if (!(q2 != q6)) return 0;
    if (!(int'(q2) + 2 != int'(q6) + 6)) return 0;
    if (!(int'(q2) - 2 != int'(q6) - 6)) return 0;
    if (!(q2 != q7)) return 0;
    if (!(int'(q2) + 2 != int'(q7) + 7)) return 0;
    if (!(int'(q2) - 2 != int'(q7) - 7)) return 0;
    if (!(q3 != q4)) return 0;
    if (!(int'(q3) + 3 != int'(q4) + 4)) return 0;
    if (!(int'(q3) - 3 != int'(q4) - 4)) return 0;
    if (!(q3 != q5)) return 0;
    if (!(int'(q3) + 3 != int'(q5) + 5)) return 0;
    if (!(int'(q3) - 3 != int'(q5) - 5)) return 0;
    if (!(q3 != q6)) return 0;
    if (!(int'(q3) + 3 != int'(q6) + 6)) return 0;
    if (!(int'(q3) - 3 != int'(q6) - 6)) return 0;
    if (!(q3 != q7)) return 0;
    if (!(int'(q3) + 3 != int'(q7) + 7)) return 0;
    if (!(int'(q3) - 3 != int'(q7) - 7)) return 0;
    if (!(q4 != q5)) return 0;
    if (!(int'(q4) + 4 != int'(q5) + 5)) return 0;
    if (!(int'(q4) - 4 != int'(q5) - 5)) return 0;
    if (!(q4 != q6)) return 0;
    if (!(int'(q4) + 4 != int'(q6) + 6)) return 0;
    if (!(int'(q4) - 4 != int'(q6) - 6)) return 0;
    if (!(q4 != q7)) return 0;
    if (!(int'(q4) + 4 != int'(q7) + 7)) return 0;
    if (!(int'(q4) - 4 != int'(q7) - 7)) return 0;
    if (!(q5 != q6)) return 0;
    if (!(int'(q5) + 5 != int'(q6) + 6)) return 0;
    if (!(int'(q5) - 5 != int'(q6) - 6)) return 0;
    if (!(q5 != q7)) return 0;
    if (!(int'(q5) + 5 != int'(q7) + 7)) return 0;
    if (!(int'(q5) - 5 != int'(q7) - 7)) return 0;
    if (!(q6 != q7)) return 0;
    if (!(int'(q6) + 6 != int'(q7) + 7)) return 0;
    if (!(int'(q6) - 6 != int'(q7) - 7)) return 0;
    return 1;
  endfunction
  constraint c_dom { q0 < 8; q1 < 8; q2 < 8; q3 < 8; q4 < 8; q5 < 8; q6 < 8; q7 < 8; }
  constraint c_r0 { q0 != q1; }
  constraint c_r1 { int'(q0) + 0 != int'(q1) + 1; }
  constraint c_r2 { int'(q0) - 0 != int'(q1) - 1; }
  constraint c_r3 { q0 != q2; }
  constraint c_r4 { int'(q0) + 0 != int'(q2) + 2; }
  constraint c_r5 { int'(q0) - 0 != int'(q2) - 2; }
  constraint c_r6 { q0 != q3; }
  constraint c_r7 { int'(q0) + 0 != int'(q3) + 3; }
  constraint c_r8 { int'(q0) - 0 != int'(q3) - 3; }
  constraint c_r9 { q0 != q4; }
  constraint c_r10 { int'(q0) + 0 != int'(q4) + 4; }
  constraint c_r11 { int'(q0) - 0 != int'(q4) - 4; }
  constraint c_r12 { q0 != q5; }
  constraint c_r13 { int'(q0) + 0 != int'(q5) + 5; }
  constraint c_r14 { int'(q0) - 0 != int'(q5) - 5; }
  constraint c_r15 { q0 != q6; }
  constraint c_r16 { int'(q0) + 0 != int'(q6) + 6; }
  constraint c_r17 { int'(q0) - 0 != int'(q6) - 6; }
  constraint c_r18 { q0 != q7; }
  constraint c_r19 { int'(q0) + 0 != int'(q7) + 7; }
  constraint c_r20 { int'(q0) - 0 != int'(q7) - 7; }
  constraint c_r21 { q1 != q2; }
  constraint c_r22 { int'(q1) + 1 != int'(q2) + 2; }
  constraint c_r23 { int'(q1) - 1 != int'(q2) - 2; }
  constraint c_r24 { q1 != q3; }
  constraint c_r25 { int'(q1) + 1 != int'(q3) + 3; }
  constraint c_r26 { int'(q1) - 1 != int'(q3) - 3; }
  constraint c_r27 { q1 != q4; }
  constraint c_r28 { int'(q1) + 1 != int'(q4) + 4; }
  constraint c_r29 { int'(q1) - 1 != int'(q4) - 4; }
  constraint c_r30 { q1 != q5; }
  constraint c_r31 { int'(q1) + 1 != int'(q5) + 5; }
  constraint c_r32 { int'(q1) - 1 != int'(q5) - 5; }
  constraint c_r33 { q1 != q6; }
  constraint c_r34 { int'(q1) + 1 != int'(q6) + 6; }
  constraint c_r35 { int'(q1) - 1 != int'(q6) - 6; }
  constraint c_r36 { q1 != q7; }
  constraint c_r37 { int'(q1) + 1 != int'(q7) + 7; }
  constraint c_r38 { int'(q1) - 1 != int'(q7) - 7; }
  constraint c_r39 { q2 != q3; }
  constraint c_r40 { int'(q2) + 2 != int'(q3) + 3; }
  constraint c_r41 { int'(q2) - 2 != int'(q3) - 3; }
  constraint c_r42 { q2 != q4; }
  constraint c_r43 { int'(q2) + 2 != int'(q4) + 4; }
  constraint c_r44 { int'(q2) - 2 != int'(q4) - 4; }
  constraint c_r45 { q2 != q5; }
  constraint c_r46 { int'(q2) + 2 != int'(q5) + 5; }
  constraint c_r47 { int'(q2) - 2 != int'(q5) - 5; }
  constraint c_r48 { q2 != q6; }
  constraint c_r49 { int'(q2) + 2 != int'(q6) + 6; }
  constraint c_r50 { int'(q2) - 2 != int'(q6) - 6; }
  constraint c_r51 { q2 != q7; }
  constraint c_r52 { int'(q2) + 2 != int'(q7) + 7; }
  constraint c_r53 { int'(q2) - 2 != int'(q7) - 7; }
  constraint c_r54 { q3 != q4; }
  constraint c_r55 { int'(q3) + 3 != int'(q4) + 4; }
  constraint c_r56 { int'(q3) - 3 != int'(q4) - 4; }
  constraint c_r57 { q3 != q5; }
  constraint c_r58 { int'(q3) + 3 != int'(q5) + 5; }
  constraint c_r59 { int'(q3) - 3 != int'(q5) - 5; }
  constraint c_r60 { q3 != q6; }
  constraint c_r61 { int'(q3) + 3 != int'(q6) + 6; }
  constraint c_r62 { int'(q3) - 3 != int'(q6) - 6; }
  constraint c_r63 { q3 != q7; }
  constraint c_r64 { int'(q3) + 3 != int'(q7) + 7; }
  constraint c_r65 { int'(q3) - 3 != int'(q7) - 7; }
  constraint c_r66 { q4 != q5; }
  constraint c_r67 { int'(q4) + 4 != int'(q5) + 5; }
  constraint c_r68 { int'(q4) - 4 != int'(q5) - 5; }
  constraint c_r69 { q4 != q6; }
  constraint c_r70 { int'(q4) + 4 != int'(q6) + 6; }
  constraint c_r71 { int'(q4) - 4 != int'(q6) - 6; }
  constraint c_r72 { q4 != q7; }
  constraint c_r73 { int'(q4) + 4 != int'(q7) + 7; }
  constraint c_r74 { int'(q4) - 4 != int'(q7) - 7; }
  constraint c_r75 { q5 != q6; }
  constraint c_r76 { int'(q5) + 5 != int'(q6) + 6; }
  constraint c_r77 { int'(q5) - 5 != int'(q6) - 6; }
  constraint c_r78 { q5 != q7; }
  constraint c_r79 { int'(q5) + 5 != int'(q7) + 7; }
  constraint c_r80 { int'(q5) - 5 != int'(q7) - 7; }
  constraint c_r81 { q6 != q7; }
  constraint c_r82 { int'(q6) + 6 != int'(q7) + 7; }
  constraint c_r83 { int'(q6) - 6 != int'(q7) - 7; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("hard.nqueens_8"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d\n", it.q0, it.q1, it.q2, it.q3, it.q4, it.q5, it.q6, it.q7);
  endfunction
endclass
