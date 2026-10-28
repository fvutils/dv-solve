// construct.inside_set: inside value sets
class sb_item;
  rand bit [7:0] a;
  rand bit [15:0] b;
  rand int c;
  constraint c_in { a inside {1, 3, 5, 7, 11, 13, 17, 19}; b inside {16'h100, 16'h200, 16'h400, 16'h800}; c inside {-7, 0, 42, 1000}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.inside_set"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a inside {1,3,5,7,11,13,17,19})) e++;
    if (!(it.b inside {16'h100,16'h200,16'h400,16'h800})) e++;
    if (!(it.c inside {-7,0,42,1000})) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.a, it.b, it.c);
  endfunction
endclass
