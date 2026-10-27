// control.soft_relaxation: soft dropped on conflict, kept otherwise
class sb_item;
  rand bit [7:0] a, b;
  constraint c_s { soft a == 8'd5; a > 8'd10; a < 8'd40; soft b inside {[0:3]}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.soft_relaxation"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a > 10 && it.a < 40)) e++;
    if (!(it.b <= 3)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.a, it.b);
  endfunction
endclass
