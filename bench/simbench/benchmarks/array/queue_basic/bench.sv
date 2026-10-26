// array.queue_basic: random-size queue
class sb_item;
  rand bit [7:0] q[$];
  function bit ok();
    if (q.size() < 2 || q.size() > 8) return 0;
    foreach (q[i]) if (q[i] <= 10 || q[i] >= 20) return 0;
    return 1;
  endfunction
  constraint c_q { q.size() inside {[2:8]}; foreach (q[i]) { q[i] > 8'd10; q[i] < 8'd20; } }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("array.queue_basic"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%p\n", it.q);
  endfunction
endclass
