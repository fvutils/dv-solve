// scenario.pcie_tlp: PCIe TLP type-dependent fields (D test_pcie_tlp)
class sb_item;
  rand bit [1:0] tlp_type;
  rand bit [4:0] length;
  rand bit [15:0] address;
  rand bit [3:0] first_be;
  rand bit [3:0] last_be;
  rand bit [7:0] tag;
  rand bit [1:0] fmt;
  function bit ok();
    if (!(length inside {[1:16]})) return 0;
    if (!(first_be != 0)) return 0;
    if ((tlp_type <= 2'd1) && !(address[1:0] == 2'd0)) return 0;
    if ((tlp_type >= 2'd2) && !(address <= 16'd1023)) return 0;
    if ((length == 5'd1) && !(last_be == 0)) return 0;
    if ((length > 5'd1) && !(last_be > 0)) return 0;
    if (!(fmt == tlp_type)) return 0;
    return 1;
  endfunction
  constraint c_dom { length inside {[1:16]}; first_be != 0; }
  constraint c_addr_align { (tlp_type <= 2'd1) -> (address[1:0] == 2'd0); }
  constraint c_cfg_range { (tlp_type >= 2'd2) -> (address <= 16'd1023); }
  constraint c_be_single { (length == 5'd1) -> (last_be == 0); }
  constraint c_be_multi { (length > 5'd1) -> (last_be > 0); }
  constraint c_fmt_type { fmt == tlp_type; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.pcie_tlp"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d\n", it.tlp_type, it.length, it.address, it.first_be, it.last_be, it.tag, it.fmt);
  endfunction
endclass
