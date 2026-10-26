// construct.range: relational and range constraints over several widths.
class sb_item;
  rand bit [7:0]  a;
  rand bit [15:0] b;
  rand bit [31:0] c;
  rand int        d;
  constraint c_a { a inside {[10:200]}; }
  constraint c_b { b > a; b < 16'd1000; }
  constraint c_c { c >= 32'd100000; c <= 32'd200000; c != 32'd150000; }
  constraint c_d { d >= -50; d <= 50; d != 0; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.range"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a >= 10 && it.a <= 200)) e++;
    if (!(it.b > it.a && it.b < 1000)) e++;
    if (!(it.c >= 100000 && it.c <= 200000 && it.c != 150000)) e++;
    if (!(it.d >= -50 && it.d <= 50 && it.d != 0)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d\n", it.a, it.b, it.c, it.d);
  endfunction
endclass
