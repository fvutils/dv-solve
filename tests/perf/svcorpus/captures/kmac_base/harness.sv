// OpenTitan hw/ip/kmac/dv/env/seq_lib/kmac_base_vseq.sv, all constraint blocks
package p;
  typedef int unsigned uint;
  localparam int TL_AW = 32;
  localparam int TL_DW = 32;
  localparam int TL_DBW = (TL_DW>>3);
  parameter int KMAC_NUM_SHARES = 2;
  parameter int KMAC_NUM_KEYS_PER_SHARE = 16;
  parameter bit [TL_AW-1:0] KMAC_FIFO_BASE = 32'h800;
  parameter bit [TL_AW-1:0] KMAC_FIFO_END = 32'hFFC;
  typedef enum int {
    KmacPrescaler = 0,
    EntropyPeriodReserved = 10,
    KmacWaitTimer = 16
  } kmac_entropy_period_e;
  // sha3_pkg
  typedef enum logic[1:0] {
    Sha3   = 2'b 00,
    Shake  = 2'b 10,
    CShake = 2'b 11
  } sha3_mode_e;
  typedef enum logic [2:0] {
    L128 = 3'b 000,
    L224 = 3'b 001,
    L256 = 3'b 010,
    L384 = 3'b 011,
    L512 = 3'b 100
  } keccak_strength_e;
  localparam int StateWidthLogic = 3;
  typedef enum logic [StateWidthLogic-1:0] {
    StIdle,
    StAbsorb,
    StSqueeze,
    StManualRun,
    StFlush,
    StError
  } sha3_st_e;
  // kmac_pkg
  typedef enum logic [2:0] {
    Key128 = 3'b 000,
    Key192 = 3'b 001,
    Key256 = 3'b 010,
    Key384 = 3'b 011,
    Key512 = 3'b 100
  } key_len_e;
  typedef enum logic [5:0] {
    CmdNone      = 6'b000000,
    CmdStart     = 6'b011101,
    CmdProcess   = 6'b101110,
    CmdManualRun = 6'b110001,
    CmdDone      = 6'b010110
  } kmac_cmd_e;
  typedef enum logic [1:0] {
    EntropyModeNone = 2'h 0,
    EntropyModeEdn  = 2'h 1,
    EntropyModeSw   = 2'h 2
  } entropy_mode_e;
  typedef enum logic [7:0] {
    ErrNone = 8'h 00,
    ErrKeyNotValid = 8'h 01,
    ErrSwPushedMsgFifo = 8'h 02,
    ErrSwIssuedCmdInAppActive = 8'h 03,
    ErrWaitTimerExpired = 8'h 04,
    ErrIncorrectEntropyMode = 8'h 05,
    ErrUnexpectedModeStrength = 8'h 06,
    ErrIncorrectFunctionName = 8'h 07,
    ErrSwCmdSequence = 8'h 08,
    ErrSwHashingWithoutEntropyReady = 8'h 09,
    ErrFatalError = 8'h C1,
    ErrPackerIntegrity = 8'h C2,
    ErrMsgFifoIntegrity = 8'h C3
  } err_code_e;
endpackage
import p::*;
class kmac_base_vseq;
  // cfg knobs (non-rand here): kmac_env_cfg.enable_masking default 1;
  // m_edn_pull_agent_cfgs[0].device_delay_max, edn_clk_freq_mhz, clk_freq_mhz
  // are rand in the cfg: typical values
  bit  cfg_enable_masking = 1;
  uint cfg_device_delay_max = 100;
  uint cfg_edn_clk_freq_mhz = 100;
  uint cfg_clk_freq_mhz = 100;

  rand bit [TL_DW-1:0] enable_intr;
  rand bit kmac_en;
  rand bit xof_en;
  rand key_len_e key_len;
  rand sha3_mode_e hash_mode;
  rand keccak_strength_e strength;
  rand bit msg_endian;
  rand bit state_endian;
  rand bit provide_sideload_key;
  rand bit reg_en_sideload;
  rand entropy_mode_e entropy_mode = EntropyModeSw;
  entropy_mode_e static_entropy_mode = EntropyModeEdn;
  rand bit entropy_fast_process;
  rand bit entropy_ready;
  rand bit en_unsupported_modestrength;
  rand bit msg_mask;
  rand int unsigned output_len;
  rand int unsigned keccak_block_size;
  rand int unsigned fname_len;
  rand int unsigned custom_str_len;
  rand byte fname_arr[];
  rand byte custom_str_arr[];
  rand bit [7:0] msg[];
  rand bit [KMAC_NUM_SHARES-1:0][KMAC_NUM_KEYS_PER_SHARE-1:0][31:0] key_share;
  rand bit [TL_AW-1:0] fifo_addr;
  rand bit [TL_DBW-1:0] data_mask;
  rand err_code_e kmac_err_type;
  rand sha3_st_e err_sw_cmd_seq_st;
  rand kmac_cmd_e err_sw_cmd_seq_cmd;
  rand bit [9:0] hash_threshold;
  rand bit hash_cnt_clr;
  rand bit entropy_req;
  rand bit entropy_timer_en;
  rand bit [EntropyPeriodReserved-1:0] prescaler_val;
  rand bit [TL_DW-KmacWaitTimer-1:0] entropy_wait_timer;

  constraint xof_en_c {
    (!kmac_en) -> (xof_en == 1'b0);
  }
  constraint hash_mode_c {
    if (kmac_en) {
      hash_mode == CShake;
    } else {
      hash_mode != CShake;
    }
  }
  constraint output_len_sha3_c {
    if (hash_mode == Sha3) {
      (strength == L224) -> (output_len == 28);
      (strength == L256) -> (output_len == 32);
      (strength == L384) -> (output_len == 48);
      (strength == L512) -> (output_len == 64);
    }
  }
  constraint strength_c {
    solve kmac_err_type before hash_mode;
    solve kmac_err_type before strength;
    if (kmac_err_type == ErrUnexpectedModeStrength) {
      (hash_mode inside {Shake, CShake}) ->
        !(strength inside {L128, L256});
      (hash_mode == Sha3) -> (strength == L128);
    } else {
      (hash_mode inside {Shake, CShake}) ->
        (strength inside {L128, L256});
      (hash_mode == Sha3) -> (strength != L128);
    }
  }
  constraint key_len_c {
    (reg_en_sideload) -> key_len == Key256;
  }
  constraint entropy_mode_c {
    if (kmac_err_type == ErrIncorrectEntropyMode) {
      !(entropy_mode inside {EntropyModeSw, EntropyModeEdn});
    } else {
      entropy_mode == static_entropy_mode;
    }
  }
  constraint keccak_block_size_c {
    (strength == L128) -> (keccak_block_size == 168);
    (strength == L224) -> (keccak_block_size == 144);
    (strength == L256) -> (keccak_block_size == 136);
    (strength == L384) -> (keccak_block_size == 104);
    (strength == L512) -> (keccak_block_size == 72);
  }
  constraint err_sw_cmd_seq_c {
    solve err_sw_cmd_seq_st before err_sw_cmd_seq_cmd;
    if (kmac_err_type == ErrSwCmdSequence) {
      if (err_sw_cmd_seq_st == StIdle) {
        err_sw_cmd_seq_cmd inside {CmdProcess, CmdManualRun, CmdDone};
      } else if (err_sw_cmd_seq_st == StAbsorb) {
        err_sw_cmd_seq_cmd inside {CmdStart, CmdManualRun, CmdDone};
      } else if (err_sw_cmd_seq_st == StSqueeze) {
        err_sw_cmd_seq_cmd inside {CmdStart, CmdProcess};
      } else if (err_sw_cmd_seq_st inside {StManualRun, StFlush}) {
        err_sw_cmd_seq_cmd != CmdNone;
      }
    }
  }
  constraint fifo_addr_c {
    fifo_addr inside {[KMAC_FIFO_BASE : KMAC_FIFO_END]};
  }
  constraint data_mask_contiguous_c {
    $countones(data_mask ^ {data_mask[TL_DBW-2:0], 1'b0}) <= 2;
  }
  constraint data_mask_c {
    $countones(data_mask) dist {
      [1 : TL_DBW] :/ 1,
      TL_DBW       :/ 1
    };
  }
  constraint prefix_len_c {
    fname_len + custom_str_len <= 36;
    fname_arr.size() == fname_len;
    custom_str_arr.size() == custom_str_len;
  }
  constraint prefix_is_char_c {
    foreach(fname_arr[i]) {
      fname_arr[i] inside {32, [65:90], [97:122]};
    }
    foreach(custom_str_arr[i]) {
      custom_str_arr[i] inside {32, [65:90], [97:122]};
    }
  }
  constraint entropy_period_c {
    if (entropy_mode == EntropyModeEdn && cfg_enable_masking) {
      if (entropy_timer_en) {
        (prescaler_val + 1) * entropy_wait_timer >
          (cfg_device_delay_max * 4 + 10) *
          (cfg_edn_clk_freq_mhz / cfg_clk_freq_mhz + 1);
      } else {
        entropy_wait_timer == 0;
      }
    }
    solve entropy_mode before prescaler_val, entropy_wait_timer;
  }
endclass
module top;
  initial begin
    kmac_base_vseq t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d %b %0d %0d %0d %0d %h %b %0d %0d %0d %h %0d %0d %0d %b %0d %0d", i, t.kmac_en,
               t.hash_mode, t.strength, t.output_len, t.keccak_block_size, t.kmac_err_type,
               t.data_mask, t.fname_len, t.custom_str_len, t.fname_arr.size(), t.fifo_addr,
               t.entropy_mode, t.prescaler_val, t.entropy_wait_timer, t.entropy_timer_en,
               t.err_sw_cmd_seq_st, t.err_sw_cmd_seq_cmd);
    end
    $finish;
  end
endmodule
