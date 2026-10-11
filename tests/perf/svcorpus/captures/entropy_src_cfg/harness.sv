// OpenTitan hw/ip/entropy_src/dv/env/entropy_src_env_cfg.sv: rand fields + constraint blocks
// (knob pcts set as entropy_src_rng_test does; UVM and agent-cfg handles stripped)
`define _DV_MUBI_DIST(VAR_, TRUE_, FALSE_, MAX_, T_WEIGHT_, F_WEIGHT_, OTHER_WEIGHT_) \
  if (TRUE_ > FALSE_) { \
    VAR_ dist {TRUE_                         := (T_WEIGHT_) * ((MAX_) - 1), \
               FALSE_                        := (F_WEIGHT_) * ((MAX_) - 1), \
               [0 : FALSE_ - 1]              := (OTHER_WEIGHT_),            \
               [FALSE_ + 1 : TRUE_ - 1]      := (OTHER_WEIGHT_),            \
               [TRUE_ + 1 : (MAX_)]          := (OTHER_WEIGHT_)};           \
  } else {                                                                  \
    VAR_ dist {TRUE_                         := (T_WEIGHT_) * ((MAX_) - 1), \
               FALSE_                        := (F_WEIGHT_) * ((MAX_) - 1), \
               [0 : TRUE_ - 1]               := (OTHER_WEIGHT_),            \
               [TRUE_ + 1 : FALSE_ - 1]      := (OTHER_WEIGHT_),            \
               [FALSE_+ 1 : (MAX_)]          := (OTHER_WEIGHT_)};           \
  }
`define DV_MUBI8_DIST(VAR_, T_WEIGHT_ = 2, F_WEIGHT_ = 2, OTHER_WEIGHT_ = 1) \
  `_DV_MUBI_DIST(VAR_, MuBi8True, MuBi8False, (1 << 8) - 1, T_WEIGHT_, F_WEIGHT_, OTHER_WEIGHT_)
package p;
  typedef int unsigned uint;
  parameter int RNG_BUS_WIDTH = 4;
  typedef enum logic [3:0] { MuBi4True = 4'h6, MuBi4False = 4'h9 } mubi4_t;
  typedef enum logic [7:0] { MuBi8True = 8'h96, MuBi8False = 8'h69 } mubi8_t;
  typedef enum int {
    sfifo_esrng_err = 0, sfifo_distr_err = 1, sfifo_observe_err = 2, sfifo_esfinal_err = 3,
    es_ack_sm_err = 4, es_main_sm_err = 5, es_cntr_err = 6, fifo_read_err = 7,
    fifo_state_err = 8, fifo_cntr_err = 9, sfifo_esrng_err_test = 10, sfifo_distr_err_test = 11,
    sfifo_observe_err_test = 12, sfifo_esfinal_err_test = 13, es_ack_sm_err_test = 14,
    es_main_sm_err_test = 15, es_cntr_err_test = 16, fifo_read_err_test = 17,
    fifo_state_err_test = 18
  } err_code_e;
  typedef enum int {
    sfifo_observe_error = 0, sfifo_esrng_error = 1, sfifo_distr_error = 2,
    sfifo_esfinal_error = 3, es_ack_sm_error = 4, es_main_sm_error = 5, es_cntr_error = 6
  } fatal_err_e;
  typedef enum int {
    window_cntr = 0, repcnt_ht_cntr = 1, repcnts_ht_cntr = 2, adaptp_ht_cntr = 3,
    bucket_ht_cntr = 4, markov_ht_cntr = 5
  } cntr_e;
  typedef enum int { repcnt_ht_fail = 0, adaptp_ht_fail = 1, bucket_ht_fail = 2,
                     markov_ht_fail = 3 } ht_fail_e;
  typedef enum int { write = 0, read = 1, state = 2 } which_fifo_err_e;
  typedef enum int { sfifo_esrng = 0, sfifo_distr = 1, sfifo_observe = 2,
                     sfifo_esfinal = 3 } which_fifo_e;
  typedef enum int { high_test = 0, low_test = 1 } which_ht_e;
  typedef enum logic [8:0] {
    Idle = 9'b011110101, BootHTRunning = 9'b111010010, BootPostHTChk = 9'b101101110,
    BootPhaseDone = 9'b010001110, StartupHTStart = 9'b000101100, StartupPhase1 = 9'b100000001,
    StartupPass1 = 9'b110100101, StartupFail1 = 9'b000010111, ContHTStart = 9'b001000000,
    ContHTRunning = 9'b110100010, FWInsertStart = 9'b011000011, FWInsertMsg = 9'b001011001,
    Sha3MsgDone = 9'b100001111, Sha3Process = 9'b011111000, Sha3Valid = 9'b010111111,
    Sha3Done = 9'b110011000, AlertState = 9'b111001101, AlertHang = 9'b111111011,
    Error = 9'b001110011
  } state_e;
endpackage
import p::*;
class entropy_src_env_cfg;
  rand int unsigned sim_duration_ms;
  uint          spurious_inject_entropy_pct = 50;
  uint          otp_en_es_fw_read_pct = 33, otp_en_es_fw_read_inval_pct = 33,
                otp_en_es_fw_over_pct = 33, otp_en_es_fw_over_inval_pct = 33;
  uint          induce_targeted_transition_pct = 75;
  rand logic [7:0]              otp_en_es_fw_read, otp_en_es_fw_over;
  rand bit                      spurious_inject_entropy;
  rand fatal_err_e      which_fatal_err;
  rand err_code_e       which_err_code;
  rand which_fifo_e     which_fifo;
  rand which_fifo_err_e which_fifo_err;
  rand ht_fail_e        which_ht_fail;
  rand cntr_e           which_cntr;
  rand which_ht_e       which_ht;
  rand state_e          which_ht_state;
  rand uint  which_cntr_replicate;
  rand uint  which_bin;
  rand bit   induce_targeted_transition;
  rand mubi4_t es_route_sw;
  rand int   fw_ov_rd_cnt;

  constraint sim_duration_ms_c {
    7 <= sim_duration_ms && sim_duration_ms <= 20;
  }
  constraint which_ht_state_c {
    which_ht_state dist {
      BootHTRunning :/ 25,
      BootPhaseDone :/ 25,
      StartupFail1  :/ 25,
      ContHTRunning :/ 25
    };
  }
  constraint otp_en_es_fw_read_c {
    `DV_MUBI8_DIST(otp_en_es_fw_read, otp_en_es_fw_read_pct,
                                      100 - otp_en_es_fw_read_pct - otp_en_es_fw_read_inval_pct,
                                      otp_en_es_fw_read_inval_pct)
  }
  constraint otp_en_es_fw_over_c {
    `DV_MUBI8_DIST(otp_en_es_fw_over, otp_en_es_fw_over_pct,
                                      100 - otp_en_es_fw_over_pct - otp_en_es_fw_over_inval_pct,
                                      otp_en_es_fw_over_inval_pct)
  }
  constraint spurious_inject_entropy_c {spurious_inject_entropy dist {
      1                         :/ spurious_inject_entropy_pct,
      0                         :/ (100 - spurious_inject_entropy_pct) };}
  constraint which_err_code_c {
    which_err_code dist {
      sfifo_esrng_err   :/ 2,
      sfifo_distr_err   :/ 2,
      sfifo_observe_err :/ 2,
      sfifo_esfinal_err :/ 2,
      es_ack_sm_err     :/ 2,
      es_main_sm_err    :/ 2,
      es_cntr_err       :/ 60,
      fifo_read_err     :/ 4,
      fifo_state_err    :/ 4,
      fifo_cntr_err     :/ 4};}
  constraint which_cntr_replicate_c {which_cntr_replicate inside {[0:RNG_BUS_WIDTH-1]};}
  int        num_bins = 2**RNG_BUS_WIDTH;
  constraint which_bin_c {which_bin inside {[0:num_bins-1]};}
  constraint which_cntr_c {which_cntr dist {
    window_cntr     :/ 1,
    repcnt_ht_cntr  :/ 4,
    repcnts_ht_cntr :/ 1,
    adaptp_ht_cntr  :/ 4,
    bucket_ht_cntr  :/ 16,
    markov_ht_cntr  :/ 4};}
  constraint which_fifo_err_c {
    which_err_code inside {sfifo_esrng_err, sfifo_distr_err, sfifo_esfinal_err} ->
      which_fifo_err inside {read, state};
    which_err_code == fifo_read_err -> which_fifo_err == read;
    which_err_code inside {fifo_state_err, fifo_cntr_err} -> which_fifo_err == state;
  }
  constraint which_fifo_c {
    which_err_code == sfifo_observe_err -> which_fifo == sfifo_observe;
    which_err_code == sfifo_esrng_err -> which_fifo == sfifo_esrng;
    which_err_code == sfifo_distr_err -> which_fifo == sfifo_distr;
    which_err_code == sfifo_esfinal_err -> which_fifo == sfifo_esfinal;
    which_err_code == fifo_cntr_err -> which_fifo inside {sfifo_observe, sfifo_esrng, sfifo_distr,
                                                          sfifo_esfinal};
  }
  constraint induce_targeted_transition_c {induce_targeted_transition dist {
    1                         :/ induce_targeted_transition_pct,
    0                         :/ (100 - induce_targeted_transition_pct) };}
  constraint es_route_sw_c {es_route_sw inside {MuBi4False, MuBi4True};}
  constraint fw_ov_rd_cnt_c {fw_ov_rd_cnt dist {
    1                         :/ 40,
    [2:127]                   :/ 20,
    128                       :/ 40 };}
endclass
module top;
  initial begin
    entropy_src_env_cfg c = new;
    for (int i = 0; i < 20; i++) begin
      if (!c.randomize()) $display("FAIL");
      $display("%0d %0d %h %h %b %0d %0d %0d %0d %0d %0d %0d %h %0d %0d %b %h %0d", i,
        c.sim_duration_ms, c.otp_en_es_fw_read, c.otp_en_es_fw_over, c.spurious_inject_entropy,
        c.which_fatal_err, c.which_err_code, c.which_fifo, c.which_fifo_err, c.which_ht_fail,
        c.which_cntr, c.which_ht, c.which_ht_state, c.which_cntr_replicate, c.which_bin,
        c.induce_targeted_transition, c.es_route_sw, c.fw_ov_rd_cnt);
    end
    $finish;
  end
endmodule
