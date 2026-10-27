// scenario.ethernet_hdr: packet header ports vs length (D test_ethernet_hdr)
class sb_item;
  rand bit [15:0] src_port;
  rand bit [15:0] dst_port;
  rand bit [10:0] length;
  function bit ok();
    if (!(length inside {[20:1500]})) return 0;
    if (!(src_port != dst_port)) return 0;
    if (!(length <= src_port)) return 0;
    if (!(dst_port <= length)) return 0;
    return 1;
  endfunction
  constraint c_dom { length inside {[20:1500]}; }
  constraint c_ports_differ { src_port != dst_port; }
  constraint c_src_fits { length <= src_port; }
  constraint c_dst_fits { dst_port <= length; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.ethernet_hdr"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d\n", it.src_port, it.dst_port, it.length);
  endfunction
endclass
