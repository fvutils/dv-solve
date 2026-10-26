// hard.graph_color_8: 3-colour an 8-node graph (cycle + 4 chords, 12 edges)
class sb_item;
  rand bit [1:0] col0;
  rand bit [1:0] col1;
  rand bit [1:0] col2;
  rand bit [1:0] col3;
  rand bit [1:0] col4;
  rand bit [1:0] col5;
  rand bit [1:0] col6;
  rand bit [1:0] col7;
  function bit ok();
    if (!(col0 < 3)) return 0;
    if (!(col1 < 3)) return 0;
    if (!(col2 < 3)) return 0;
    if (!(col3 < 3)) return 0;
    if (!(col4 < 3)) return 0;
    if (!(col5 < 3)) return 0;
    if (!(col6 < 3)) return 0;
    if (!(col7 < 3)) return 0;
    if (!(col0 != col1)) return 0;
    if (!(col1 != col2)) return 0;
    if (!(col2 != col3)) return 0;
    if (!(col3 != col4)) return 0;
    if (!(col4 != col5)) return 0;
    if (!(col5 != col6)) return 0;
    if (!(col6 != col7)) return 0;
    if (!(col7 != col0)) return 0;
    if (!(col0 != col2)) return 0;
    if (!(col2 != col4)) return 0;
    if (!(col4 != col6)) return 0;
    if (!(col6 != col0)) return 0;
    return 1;
  endfunction
  constraint c_dom { col0 < 3; col1 < 3; col2 < 3; col3 < 3; col4 < 3; col5 < 3; col6 < 3; col7 < 3; }
  constraint c_r0 { col0 != col1; }
  constraint c_r1 { col1 != col2; }
  constraint c_r2 { col2 != col3; }
  constraint c_r3 { col3 != col4; }
  constraint c_r4 { col4 != col5; }
  constraint c_r5 { col5 != col6; }
  constraint c_r6 { col6 != col7; }
  constraint c_r7 { col7 != col0; }
  constraint c_r8 { col0 != col2; }
  constraint c_r9 { col2 != col4; }
  constraint c_r10 { col4 != col6; }
  constraint c_r11 { col6 != col0; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("hard.graph_color_8"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d\n", it.col0, it.col1, it.col2, it.col3, it.col4, it.col5, it.col6, it.col7);
  endfunction
endclass
