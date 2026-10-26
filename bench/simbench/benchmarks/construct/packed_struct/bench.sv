// construct.packed_struct: packed struct as a whole rand member
typedef struct packed { bit en; bit [2:0] mode; bit [7:0] div; bit [7:0] thr; } cfg_t;
class sb_item;
  rand cfg_t c;
  constraint c_s { c.en -> c.div > 0; c.div < 8'd100; c.mode inside {[1:5]}; c.thr == c.div / 4; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.packed_struct"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(!it.c.en || it.c.div > 0)) e++;
    if (!(it.c.div < 100)) e++;
    if (!(it.c.mode >= 1 && it.c.mode <= 5)) e++;
    if (!(it.c.thr == it.c.div/4)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d\n", it.c.en, it.c.mode, it.c.div, it.c.thr);
  endfunction
endclass
