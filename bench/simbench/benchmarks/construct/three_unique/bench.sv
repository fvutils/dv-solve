// construct.three_unique: unique over three scalars
class sb_item;
  rand bit [7:0] a, b, c;
  constraint c_u { unique {a, b, c}; a inside {[0:7]}; b inside {[0:7]}; c inside {[0:7]}; a + b + c == 8'd12; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.three_unique"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a!=it.b && it.a!=it.c && it.b!=it.c)) e++;
    if (!(it.a<8 && it.b<8 && it.c<8)) e++;
    if (!(it.a+it.b+it.c == 12)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.a, it.b, it.c);
  endfunction
endclass
