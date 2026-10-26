// construct.inside_ranges: inside multiple ranges, negated inside
class sb_item;
  rand bit [31:0] a;
  rand bit [7:0] b;
  constraint c_in { a inside {[32'h1000:32'h1fff], [32'h8000_0000:32'h8000_ffff], 32'hffff_0000}; b inside {[0:9], [100:109], [250:255]}; !(b inside {[5:6]}); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("construct.inside_ranges"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.a inside {[32'h1000:32'h1fff],[32'h80000000:32'h8000ffff],32'hffff0000})) e++;
    if (!(it.b inside {[0:9],[100:109],[250:255]})) e++;
    if (!(!(it.b inside {[5:6]}))) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d\n", it.a, it.b);
  endfunction
endclass
