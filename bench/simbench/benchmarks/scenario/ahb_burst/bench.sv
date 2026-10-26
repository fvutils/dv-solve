// scenario.ahb_burst: AHB INCR burst that must not cross a 4 KB boundary
class sb_item;
  rand bit [31:0] haddr;
  rand bit [2:0] hsize;
  rand bit [2:0] hburst;
  rand bit [4:0] beats;
  rand bit [12:0] bytes;
  function bit ok();
    if (!(hsize <= 3'd2)) return 0;
    if (!(hburst inside {3'd1,3'd3,3'd5,3'd7})) return 0;
    if ((hburst == 3'd1) && !(beats inside {[1:16]})) return 0;
    if ((hburst == 3'd3) && !(beats == 4)) return 0;
    if ((hburst == 3'd5) && !(beats == 8)) return 0;
    if ((hburst == 3'd7) && !(beats == 16)) return 0;
    if (!(bytes == 13'(beats) << hsize)) return 0;
    if (!((haddr & ((32'd1 << hsize) - 1)) == 0)) return 0;
    if (!({20'd0, haddr[11:0]} + bytes <= 32'd4096)) return 0;
    return 1;
  endfunction
  constraint c_dom { hsize <= 3'd2; hburst inside {3'd1,3'd3,3'd5,3'd7}; }
  constraint c_beats1 { (hburst == 3'd1) -> (beats inside {[1:16]}); }
  constraint c_beats4 { (hburst == 3'd3) -> (beats == 4); }
  constraint c_beats8 { (hburst == 3'd5) -> (beats == 8); }
  constraint c_beats16 { (hburst == 3'd7) -> (beats == 16); }
  constraint c_bytes { bytes == 13'(beats) << hsize; }
  constraint c_align { (haddr & ((32'd1 << hsize) - 1)) == 0; }
  constraint c_4k { {20'd0, haddr[11:0]} + bytes <= 32'd4096; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.ahb_burst"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d\n", it.haddr, it.hsize, it.hburst, it.beats, it.bytes);
  endfunction
endclass
