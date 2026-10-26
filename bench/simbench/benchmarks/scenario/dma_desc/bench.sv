// scenario.dma_desc: DMA descriptor: aligned src/dst/len, chained next pointer
class sb_item;
  rand bit [31:0] src;
  rand bit [31:0] dst;
  rand bit [15:0] len;
  rand bit [1:0] width;
  rand bit chain;
  rand bit [31:0] next;
  function bit ok();
    if (!(len inside {[1:4096]})) return 0;
    if (!(src inside {[32'h1000_0000:32'h1FFF_FFFF]})) return 0;
    if (!(dst inside {[32'h2000_0000:32'h2FFF_FFFF]})) return 0;
    if (!((src & ((32'd1 << width) - 1)) == 0 && (dst & ((32'd1 << width) - 1)) == 0 && (len & ((16'd1 << width) - 1)) == 0)) return 0;
    if (!(src + len <= 32'h2000_0000)) return 0;
    if (!(dst + len <= 32'h3000_0000)) return 0;
    if ((chain) && !(next[3:0] == 0 && next inside {[32'h0010_0000:32'h001F_FFF0]})) return 0;
    if ((!chain) && !(next == 0)) return 0;
    return 1;
  endfunction
  constraint c_dom { len inside {[1:4096]}; src inside {[32'h1000_0000:32'h1FFF_FFFF]}; dst inside {[32'h2000_0000:32'h2FFF_FFFF]}; }
  constraint c_align { (src & ((32'd1 << width) - 1)) == 0 && (dst & ((32'd1 << width) - 1)) == 0 && (len & ((16'd1 << width) - 1)) == 0; }
  constraint c_src_end { src + len <= 32'h2000_0000; }
  constraint c_dst_end { dst + len <= 32'h3000_0000; }
  constraint c_chain { (chain) -> (next[3:0] == 0 && next inside {[32'h0010_0000:32'h001F_FFF0]}); }
  constraint c_nochain { (!chain) -> (next == 0); }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.dma_desc"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d\n", it.src, it.dst, it.len, it.width, it.chain, it.next);
  endfunction
endclass
