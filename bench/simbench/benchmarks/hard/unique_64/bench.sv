// hard.unique_64: 64 x 8-bit array, unique {} (2016 implicit !=)
class sb_item;
  rand bit [7:0] u[64];
  function bit ok();
    for (int i = 0; i < 64; i++) for (int j = i+1; j < 64; j++) if (u[i] == u[j]) return 0;
    return 1;
  endfunction
  constraint c_u { unique {u}; }
endclass
class sb_bench_impl extends simbench_pkg::sb_bench;
  sb_item it = new();
  function new(); super.new("hard.unique_64"); endfunction
  virtual function bit do_randomize(); return it.randomize(); endfunction
  virtual function int check();
    int e = 0;
    if (!(it.ok())) e++;
    return e;
  endfunction
  virtual function void dump(int fd);
    $fwrite(fd, "%0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d\n", it.u[0], it.u[1], it.u[2], it.u[3], it.u[4], it.u[5], it.u[6], it.u[7], it.u[8], it.u[9], it.u[10], it.u[11], it.u[12], it.u[13], it.u[14], it.u[15], it.u[16], it.u[17], it.u[18], it.u[19], it.u[20], it.u[21], it.u[22], it.u[23], it.u[24], it.u[25], it.u[26], it.u[27], it.u[28], it.u[29], it.u[30], it.u[31], it.u[32], it.u[33], it.u[34], it.u[35], it.u[36], it.u[37], it.u[38], it.u[39], it.u[40], it.u[41], it.u[42], it.u[43], it.u[44], it.u[45], it.u[46], it.u[47], it.u[48], it.u[49], it.u[50], it.u[51], it.u[52], it.u[53], it.u[54], it.u[55], it.u[56], it.u[57], it.u[58], it.u[59], it.u[60], it.u[61], it.u[62], it.u[63]);
  endfunction
endclass
