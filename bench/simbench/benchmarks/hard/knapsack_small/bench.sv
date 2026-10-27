// hard.knapsack_small: 0/1 knapsack, 6 items: weight <= 20, value >= 22
class sb_item;
  rand bit t0;
  rand bit t1;
  rand bit t2;
  rand bit t3;
  rand bit t4;
  rand bit t5;
  function bit ok();
    if (!(int'(t0)*3 + int'(t1)*4 + int'(t2)*5 + int'(t3)*6 + int'(t4)*7 + int'(t5)*8 <= 20)) return 0;
    if (!(int'(t0)*4 + int'(t1)*5 + int'(t2)*6 + int'(t3)*7 + int'(t4)*8 + int'(t5)*9 >= 22)) return 0;
    return 1;
  endfunction
  constraint c_dom {  }
  constraint c_r0 { int'(t0)*3 + int'(t1)*4 + int'(t2)*5 + int'(t3)*6 + int'(t4)*7 + int'(t5)*8 <= 20; }
  constraint c_r1 { int'(t0)*4 + int'(t1)*5 + int'(t2)*6 + int'(t3)*7 + int'(t4)*8 + int'(t5)*9 >= 22; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("hard.knapsack_small"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d\n", it.t0, it.t1, it.t2, it.t3, it.t4, it.t5);
  endfunction
endclass
