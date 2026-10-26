// control.rand_mode: rand_mode(0) holds a; b follows it
class sb_item;
  rand bit [7:0] a, b;
  constraint c_r { b == a + 8'd1; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("control.rand_mode"); endfunction
  virtual function bit do_randomize(); it.a.rand_mode(0); it.a = it.a + 8'd3; return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.b == 8'(it.a + 1))) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.a, it.b);
  endfunction
endclass
