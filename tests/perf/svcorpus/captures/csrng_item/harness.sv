// OpenTitan hw/dv/sv/csrng_agent/csrng_item.sv: c_clen, c_cmd_data, c_flags, c_glen
package p;
  typedef enum logic [2:0] {
    INV  = 3'h0,
    INS  = 3'h1,
    RES  = 3'h2,
    GEN  = 3'h3,
    UPD  = 3'h4,
    UNI  = 3'h5,
    GENB = 3'h6,
    GENU = 3'h7
  } acmd_e;
  typedef enum logic [3:0] {
    MuBi4True = 4'h6, // enabled
    MuBi4False = 4'h9  // disabled
  } mubi4_t;
endpackage
import p::*;
class csrng_item;
  rand acmd_e       acmd;
  rand mubi4_t      flags;
  rand bit [3:0]    clen;
  rand bit [11:0]   glen;
  rand bit [31:0]   cmd_data_q[$];

  constraint c_clen {
    clen inside {[0:12]};
  }

  constraint c_cmd_data {
    solve clen before cmd_data_q;
    cmd_data_q.size() == clen;
  }

  constraint c_flags {
    flags inside {MuBi4True, MuBi4False};
  }

  constraint c_glen {
    glen dist {
      // Note that 0 isn't supported by the implementation, see #23846.
      [1:32]      :/ 75,
      [33:128]    :/ 10,
      [129:1024]  :/ 5,
      [1025:4094] :/ 5,
      4095        := 5
    };
  }
endclass
module top;
  initial begin
    csrng_item t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d %0d %h %0d %0d %0d", i, t.acmd, t.flags, t.clen, t.glen, t.cmd_data_q.size());
    end
    $finish;
  end
endmodule
