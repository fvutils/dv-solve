// construct.inside_vars: `x inside {a, b, ...}` over a few non-rand members,
// which change between calls (riscv-dv picks registers and bases this way).
// The OR of 2-16 `x == v` is a DisjClause; until S12.3 its values gave x no
// domain, so the search drew a 32-bit x blind and refuted one value at a time.
class sb_item;
  rand bit [31:0] addr;
  rand int        rd;
  bit [31:0] b0, b1, b2, b3, b4, b5;
  int        r0, r1, r2;
  constraint c_in { addr inside {b0, b1, b2, b3, b4, b5}; rd inside {r0, r1, r2}; }

  // New contents for call n: a multiplicative hash, so every call differs.
  function void load(int unsigned n);
    bit [31:0] h = n * 32'h9E3779B1;
    b0 = h; b1 = h ^ 32'h1000; b2 = h + 32'h40; b3 = ~h; b4 = h >> 3; b5 = 32'hFFFF_0000;
    r0 = int'(h[4:0]); r1 = int'(h[9:5]) - 16; r2 = 31;
  endfunction
  function bit ok();
    return addr inside {b0, b1, b2, b3, b4, b5} && rd inside {r0, r1, r2};
  endfunction
endclass

class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  int unsigned n;
  function new(); super.new("construct.inside_vars"); endfunction
  virtual function bit do_randomize();
    it.load(++n);
    return it.randomize();
  endfunction
  virtual function int check();
    return it.ok() ? 0 : 1;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.addr, it.rd);
  endfunction
endclass
