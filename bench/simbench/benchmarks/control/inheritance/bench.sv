// control.inheritance: derived class overrides a base constraint
class sb_base;
  rand bit [7:0] a;
  constraint c_base { a < 8'd100; }
endclass
class sb_item extends sb_base;
  rand bit [7:0] b;
  constraint c_base { a < 8'd50; }
  constraint c_der { b > a; b < 8'd60; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.inheritance"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a < 50)) e++;
    if (!(it.b > it.a && it.b < 60)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.a, it.b);
  endfunction
endclass
