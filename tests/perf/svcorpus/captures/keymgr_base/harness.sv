// OpenTitan hw/ip/keymgr/dv/env/seq_lib/keymgr_base_vseq.sv: rand fields + constraint blocks
// (UVM stripped; keymgr_pkg enums inlined)
package keymgr_pkg;
  typedef enum logic [1:0] { None, Aes, Kmac, Otbn } keymgr_key_dest_e;
  typedef enum logic [2:0] {
    OpAdvance = 0, OpGenId = 1, OpGenSwOut = 2, OpGenHwOut = 3, OpDisable = 4
  } keymgr_ops_e;
endpackage
class keymgr_base_vseq;
  rand bit do_op_before_init;
  rand keymgr_pkg::keymgr_ops_e gen_operation;
  rand keymgr_pkg::keymgr_key_dest_e key_dest;
  rand bit do_rand_otp_key;
  rand bit do_invalid_otp_key;
  rand bit is_key_version_err;
  constraint is_key_version_err_c {
    is_key_version_err == 0;
  }
  constraint otp_key_c {
    do_rand_otp_key == 0;
    do_invalid_otp_key == 0;
  }
  constraint gen_operation_c {
    gen_operation inside {keymgr_pkg::OpGenId, keymgr_pkg::OpGenSwOut, keymgr_pkg::OpGenHwOut};
  }
endclass
module top;
  initial begin
    keymgr_base_vseq s = new;
    for (int i = 0; i < 20; i++) begin
      if (!s.randomize()) $display("FAIL");
      $display("%0d %b %0d %0d %b%b%b", i, s.do_op_before_init, s.gen_operation, s.key_dest,
               s.do_rand_otp_key, s.do_invalid_otp_key, s.is_key_version_err);
    end
    $finish;
  end
endmodule
