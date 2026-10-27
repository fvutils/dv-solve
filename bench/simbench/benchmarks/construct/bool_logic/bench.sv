// construct.bool_logic: boolean combinations
class sb_item;
  rand bit a, b, c, d, e;
  rand bit [7:0] x;
  constraint c_l { (a || b) && !(a && b); c == (a ^ d); (e && !c) || x > 8'd200; (b && d) -> x < 8'd220; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.bool_logic"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!((it.a || it.b) && !(it.a && it.b))) e++;
    if (!(it.c == (it.a ^ it.d))) e++;
    if (!((it.e && !it.c) || it.x > 200)) e++;
    if (!(!(it.b && it.d) || it.x < 220)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d\n", it.a, it.b, it.c, it.d, it.e, it.x);
  endfunction
endclass
