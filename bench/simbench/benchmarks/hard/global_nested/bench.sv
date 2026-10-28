// hard.global_nested: constraints in top class over 2-level nested rand objects (V t_constraint_global_nested_*)
class sb_leaf;
  rand bit [7:0] v;
endclass
class sb_mid;
  rand sb_leaf l = new();
  rand bit [7:0] w;
endclass
class sb_item;
  rand sb_mid m0 = new();
  rand sb_mid m1 = new();
  rand bit [7:0] x;
  function bit ok();
    if (!(x == 8'(m0.l.v + m1.l.v))) return 0;
    if (!(m0.w > m0.l.v)) return 0;
    if (!(m1.w < m1.l.v)) return 0;
    if (!(m0.l.v < m1.l.v)) return 0;
    if (!(x inside {[100:200]})) return 0;
    if (!(m0.w != m1.w)) return 0;
    return 1;
  endfunction
  constraint c_r0 { x == 8'(m0.l.v + m1.l.v); }
  constraint c_r1 { m0.w > m0.l.v; }
  constraint c_r2 { m1.w < m1.l.v; }
  constraint c_r3 { m0.l.v < m1.l.v; }
  constraint c_r4 { x inside {[100:200]}; }
  constraint c_r5 { m0.w != m1.w; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("hard.global_nested"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d\n", it.m0.l.v, it.m0.w, it.m1.l.v, it.m1.w, it.x);
  endfunction
endclass
