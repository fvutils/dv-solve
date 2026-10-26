// construct.struct_fields: constraints on packed struct members
typedef struct packed { bit [3:0] kind; bit [7:0] len; bit [7:0] src; bit [7:0] dst; } hdr_t;
class sb_item;
  rand hdr_t h;
  rand bit [15:0] crc;
  constraint c_s { h.len inside {[4:60]}; h.kind != 0; h.dst != h.src; crc == {h.dst, h.src}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.struct_fields"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.h.len >= 4 && it.h.len <= 60)) e++;
    if (!(it.h.kind != 0)) e++;
    if (!(it.h.dst != it.h.src)) e++;
    if (!(it.crc == {it.h.dst, it.h.src})) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d\n", it.h.kind, it.h.len, it.h.src, it.h.dst, it.crc);
  endfunction
endclass
