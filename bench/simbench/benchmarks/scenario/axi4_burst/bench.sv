// scenario.axi4_burst: AXI4 burst legality (from tests/bench/test_axi4_burst.py).
// burst: 0=FIXED 1=INCR 2=WRAP; addresses in words, 4KB page = 1024 words.
class sb_item;
  rand bit [9:0]  addr_word;
  rand bit [3:0]  addr_page;
  rand bit [7:0]  len_m1;
  rand bit [1:0]  burst;
  rand bit [10:0] end_word;
  constraint c_burst    { burst <= 2; }
  constraint c_end_word { end_word == 11'(addr_word) + 11'(len_m1) + 11'd1; }
  constraint c_no_cross { end_word <= 1024; }
  constraint c_wrap_len { burst == 2 -> len_m1 inside {1, 3, 7, 15}; }
  constraint c_fixed_len{ burst == 0 -> len_m1 <= 15; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("scenario.axi4_burst"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (it.burst > 2) e++;
    if (int'(it.end_word) != int'(it.addr_word) + int'(it.len_m1) + 1) e++;
    if (it.end_word > 1024) e++;
    if (it.burst == 2 && !(it.len_m1 inside {1, 3, 7, 15})) e++;
    if (it.burst == 0 && it.len_m1 > 15) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d\n", it.addr_word, it.addr_page, it.len_m1, it.burst, it.end_word);
  endfunction
endclass
