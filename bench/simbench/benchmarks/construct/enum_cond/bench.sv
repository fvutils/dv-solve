// construct.enum_cond: enum-conditioned constraints
typedef enum {C_RD, C_WR, C_CFG} cmd_e;
class sb_item;
  rand cmd_e cmd;
  rand bit [7:0] len;
  rand bit [31:0] addr;
  constraint c_e { (cmd == C_RD) -> len inside {[1:16]}; (cmd == C_WR) -> len inside {[1:64]}; (cmd == C_CFG) -> (len == 1 && addr < 32'h100); addr[1:0] == 2'b00; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.enum_cond"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.cmd != C_RD || (it.len>=1 && it.len<=16))) e++;
    if (!(it.cmd != C_WR || (it.len>=1 && it.len<=64))) e++;
    if (!(it.cmd != C_CFG || (it.len==1 && it.addr < 32'h100))) e++;
    if (!(it.addr[1:0]==0)) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.cmd, it.len, it.addr);
  endfunction
endclass
