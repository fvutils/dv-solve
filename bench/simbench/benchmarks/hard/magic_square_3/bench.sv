// hard.magic_square_3: 3x3 magic square: distinct 1..9, 8 line sums == 15
class sb_item;
  rand bit [7:0] m0;
  rand bit [7:0] m1;
  rand bit [7:0] m2;
  rand bit [7:0] m3;
  rand bit [7:0] m4;
  rand bit [7:0] m5;
  rand bit [7:0] m6;
  rand bit [7:0] m7;
  rand bit [7:0] m8;
  function bit ok();
    if (!(m0 inside {[1:9]})) return 0;
    if (!(m1 inside {[1:9]})) return 0;
    if (!(m2 inside {[1:9]})) return 0;
    if (!(m3 inside {[1:9]})) return 0;
    if (!(m4 inside {[1:9]})) return 0;
    if (!(m5 inside {[1:9]})) return 0;
    if (!(m6 inside {[1:9]})) return 0;
    if (!(m7 inside {[1:9]})) return 0;
    if (!(m8 inside {[1:9]})) return 0;
    if (!(m0 != m1)) return 0;
    if (!(m0 != m2)) return 0;
    if (!(m0 != m3)) return 0;
    if (!(m0 != m4)) return 0;
    if (!(m0 != m5)) return 0;
    if (!(m0 != m6)) return 0;
    if (!(m0 != m7)) return 0;
    if (!(m0 != m8)) return 0;
    if (!(m1 != m2)) return 0;
    if (!(m1 != m3)) return 0;
    if (!(m1 != m4)) return 0;
    if (!(m1 != m5)) return 0;
    if (!(m1 != m6)) return 0;
    if (!(m1 != m7)) return 0;
    if (!(m1 != m8)) return 0;
    if (!(m2 != m3)) return 0;
    if (!(m2 != m4)) return 0;
    if (!(m2 != m5)) return 0;
    if (!(m2 != m6)) return 0;
    if (!(m2 != m7)) return 0;
    if (!(m2 != m8)) return 0;
    if (!(m3 != m4)) return 0;
    if (!(m3 != m5)) return 0;
    if (!(m3 != m6)) return 0;
    if (!(m3 != m7)) return 0;
    if (!(m3 != m8)) return 0;
    if (!(m4 != m5)) return 0;
    if (!(m4 != m6)) return 0;
    if (!(m4 != m7)) return 0;
    if (!(m4 != m8)) return 0;
    if (!(m5 != m6)) return 0;
    if (!(m5 != m7)) return 0;
    if (!(m5 != m8)) return 0;
    if (!(m6 != m7)) return 0;
    if (!(m6 != m8)) return 0;
    if (!(m7 != m8)) return 0;
    if (!(m0 + m1 + m2 == 15)) return 0;
    if (!(m3 + m4 + m5 == 15)) return 0;
    if (!(m6 + m7 + m8 == 15)) return 0;
    if (!(m0 + m3 + m6 == 15)) return 0;
    if (!(m1 + m4 + m7 == 15)) return 0;
    if (!(m2 + m5 + m8 == 15)) return 0;
    if (!(m0 + m4 + m8 == 15)) return 0;
    if (!(m2 + m4 + m6 == 15)) return 0;
    return 1;
  endfunction
  constraint c_dom { m0 inside {[1:9]}; m1 inside {[1:9]}; m2 inside {[1:9]}; m3 inside {[1:9]}; m4 inside {[1:9]}; m5 inside {[1:9]}; m6 inside {[1:9]}; m7 inside {[1:9]}; m8 inside {[1:9]}; }
  constraint c_r0 { m0 != m1; }
  constraint c_r1 { m0 != m2; }
  constraint c_r2 { m0 != m3; }
  constraint c_r3 { m0 != m4; }
  constraint c_r4 { m0 != m5; }
  constraint c_r5 { m0 != m6; }
  constraint c_r6 { m0 != m7; }
  constraint c_r7 { m0 != m8; }
  constraint c_r8 { m1 != m2; }
  constraint c_r9 { m1 != m3; }
  constraint c_r10 { m1 != m4; }
  constraint c_r11 { m1 != m5; }
  constraint c_r12 { m1 != m6; }
  constraint c_r13 { m1 != m7; }
  constraint c_r14 { m1 != m8; }
  constraint c_r15 { m2 != m3; }
  constraint c_r16 { m2 != m4; }
  constraint c_r17 { m2 != m5; }
  constraint c_r18 { m2 != m6; }
  constraint c_r19 { m2 != m7; }
  constraint c_r20 { m2 != m8; }
  constraint c_r21 { m3 != m4; }
  constraint c_r22 { m3 != m5; }
  constraint c_r23 { m3 != m6; }
  constraint c_r24 { m3 != m7; }
  constraint c_r25 { m3 != m8; }
  constraint c_r26 { m4 != m5; }
  constraint c_r27 { m4 != m6; }
  constraint c_r28 { m4 != m7; }
  constraint c_r29 { m4 != m8; }
  constraint c_r30 { m5 != m6; }
  constraint c_r31 { m5 != m7; }
  constraint c_r32 { m5 != m8; }
  constraint c_r33 { m6 != m7; }
  constraint c_r34 { m6 != m8; }
  constraint c_r35 { m7 != m8; }
  constraint c_r36 { m0 + m1 + m2 == 15; }
  constraint c_r37 { m3 + m4 + m5 == 15; }
  constraint c_r38 { m6 + m7 + m8 == 15; }
  constraint c_r39 { m0 + m3 + m6 == 15; }
  constraint c_r40 { m1 + m4 + m7 == 15; }
  constraint c_r41 { m2 + m5 + m8 == 15; }
  constraint c_r42 { m0 + m4 + m8 == 15; }
  constraint c_r43 { m2 + m4 + m6 == 15; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("hard.magic_square_3"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d %0d\n", it.m0, it.m1, it.m2, it.m3, it.m4, it.m5, it.m6, it.m7, it.m8);
  endfunction
endclass
