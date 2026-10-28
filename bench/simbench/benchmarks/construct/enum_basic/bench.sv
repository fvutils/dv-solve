// construct.enum_basic: enum value constraints
typedef enum bit [2:0] {OP_NOP, OP_ADD, OP_SUB, OP_LD, OP_ST} op_e;
typedef enum bit [1:0] {M_OFF, M_FAST, M_SLOW} mode_e;
class sb_item;
  rand op_e op;
  rand mode_e md;
  constraint c_e { op != OP_NOP; md inside {M_FAST, M_SLOW}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.enum_basic"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.op != OP_NOP)) e++;
    if (!(it.md == M_FAST || it.md == M_SLOW)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.op, it.md);
  endfunction
endclass
