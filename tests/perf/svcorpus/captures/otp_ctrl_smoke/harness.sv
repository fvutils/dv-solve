// OpenTitan hw/top_earlgrey/ip_autogen/otp_ctrl/dv/env/seq_lib/otp_ctrl_smoke_vseq.sv
// (+ the rand vars of dv_base_vseq / otp_ctrl_base_vseq its constraints override)
`define PART_CONTENT_RANGE(i) \
    {[PartInfo[``i``].offset : (PartInfo[``i``].offset + PartInfo[``i``].size - DIGEST_SIZE - 1)]}
package p;
  typedef int unsigned uint;
  parameter int TL_DW = 32;
  parameter int OtpByteAddrWidth = 11;
  parameter int NumPart = 11;
  parameter uint DIGEST_SIZE = 8;
  parameter int ConstSelWidth = 2;
  typedef enum logic [ConstSelWidth-1:0] {
    Secret0Key,
    Secret1Key,
    Secret2Key
  } key_sel_e;
  typedef enum logic [1:0] {
    Unbuffered,
    Buffered,
    LifeCycle
  } part_variant_e;
  typedef struct packed {
    part_variant_e variant;
    logic [OtpByteAddrWidth-1:0] offset;
    logic [OtpByteAddrWidth-1:0] size;
    key_sel_e key_sel;
    logic secret;
    logic sw_digest;
    logic hw_digest;
    logic write_lock;
    logic read_lock;
    logic integrity;
    logic iskeymgr_creator;
    logic iskeymgr_owner;
  } part_info_t;
  localparam part_info_t PartInfo [NumPart] = '{
    '{Unbuffered, 11'd0,    64,  key_sel_e'('0), 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 1'b0, 1'b0, 1'b0},
    '{Unbuffered, 11'd64,   368, key_sel_e'('0), 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 1'b0},
    '{Unbuffered, 11'd432,  712, key_sel_e'('0), 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 1'b0},
    '{Unbuffered, 11'd1144, 472, key_sel_e'('0), 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 1'b0},
    '{Unbuffered, 11'd1616, 40,  key_sel_e'('0), 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 1'b0},
    '{Buffered,   11'd1656, 72,  key_sel_e'('0), 1'b0, 1'b0, 1'b1, 1'b1, 1'b0, 1'b1, 1'b0, 1'b0},
    '{Buffered,   11'd1728, 16,  key_sel_e'('0), 1'b0, 1'b0, 1'b1, 1'b1, 1'b0, 1'b1, 1'b0, 1'b0},
    '{Buffered,   11'd1744, 40,  Secret0Key,     1'b1, 1'b0, 1'b1, 1'b1, 1'b1, 1'b1, 1'b0, 1'b0},
    '{Buffered,   11'd1784, 88,  Secret1Key,     1'b1, 1'b0, 1'b1, 1'b1, 1'b1, 1'b1, 1'b0, 1'b0},
    '{Buffered,   11'd1872, 88,  Secret2Key,     1'b1, 1'b0, 1'b1, 1'b1, 1'b1, 1'b1, 1'b1, 1'b0},
    '{LifeCycle,  11'd1960, 88,  key_sel_e'('0), 1'b0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b1, 1'b0, 1'b0}
  };
  // Changed: Verilator 5.052 lowers PartInfo[part_idx].secret (rand index into a
  // localparam array of packed structs) to a malformed (select <bv const> i),
  // and a bit-select tbl[part_idx] of a constant vector to tbl[<part_idx's
  // current value>]: the constraint shifts this packed table of the .secret
  // fields by part_idx instead.
  localparam bit [NumPart-1:0] PartInfoSecret = {
    PartInfo[10].secret, PartInfo[9].secret, PartInfo[8].secret, PartInfo[7].secret,
    PartInfo[6].secret, PartInfo[5].secret, PartInfo[4].secret, PartInfo[3].secret,
    PartInfo[2].secret, PartInfo[1].secret, PartInfo[0].secret};
  typedef enum {
    VendorTestIdx,
    CreatorSwCfgIdx,
    OwnerSwCfgIdx,
    RotCreatorAuthCodesignIdx,
    RotCreatorAuthStateIdx,
    HwCfg0Idx,
    HwCfg1Idx,
    Secret0Idx,
    Secret1Idx,
    Secret2Idx,
    LifeCycleIdx,
    DaiIdx,
    LciIdx,
    KdiIdx,
    NumAgentsIdx
  } part_idx_e;
  typedef enum bit [1:0] {
    OtpNoEccErr,
    OtpEccCorrErr,
    OtpEccUncorrErr
  } otp_ecc_err_e;
endpackage
import p::*;
class otp_ctrl_smoke_vseq;
  // dv_base_vseq / otp_ctrl_base_vseq
  rand uint num_trans;
  rand int apply_reset_during_pwr_init_cycles;
  // cfg.smoke_test (dv_base_env_cfg default)
  bit cfg_smoke_test = 0;

  rand bit                           do_req_keys, do_lc_trans;
  rand bit                           access_locked_parts;
  rand bit                           rand_wr, rand_rd, rd_sw_tlul_rd;
  rand bit [TL_DW-1:0]               dai_addr;
  rand bit [TL_DW-1:0]               wdata0, wdata1;
  rand int                           num_dai_op;
  rand part_idx_e                    part_idx;
  rand bit                           check_regwen_val, check_trigger_regwen_val;
  rand bit [TL_DW-1:0]               check_timeout_val;
  rand bit [1:0]                     check_trigger_val;
  rand otp_ecc_err_e                 ecc_otp_err, ecc_chk_err;

  constraint no_access_err_c {access_locked_parts == 0;}
  constraint partition_index_c {part_idx inside {[0:LifeCycleIdx-1]};}
  constraint dai_wr_legal_addr_c {
    if (part_idx == VendorTestIdx)
        dai_addr inside `PART_CONTENT_RANGE(VendorTestIdx);
    if (part_idx == CreatorSwCfgIdx)
        dai_addr inside `PART_CONTENT_RANGE(CreatorSwCfgIdx);
    if (part_idx == OwnerSwCfgIdx)
        dai_addr inside `PART_CONTENT_RANGE(OwnerSwCfgIdx);
    if (part_idx == RotCreatorAuthCodesignIdx)
        dai_addr inside `PART_CONTENT_RANGE(RotCreatorAuthCodesignIdx);
    if (part_idx == RotCreatorAuthStateIdx)
        dai_addr inside `PART_CONTENT_RANGE(RotCreatorAuthStateIdx);
    if (part_idx == HwCfg0Idx)
        dai_addr inside `PART_CONTENT_RANGE(HwCfg0Idx);
    if (part_idx == HwCfg1Idx)
        dai_addr inside `PART_CONTENT_RANGE(HwCfg1Idx);
    if (part_idx == Secret0Idx)
        dai_addr inside `PART_CONTENT_RANGE(Secret0Idx);
    if (part_idx == Secret1Idx)
        dai_addr inside `PART_CONTENT_RANGE(Secret1Idx);
    if (part_idx == Secret2Idx)
        dai_addr inside `PART_CONTENT_RANGE(Secret2Idx);
    if (part_idx == LifeCycleIdx)
        dai_addr inside `PART_CONTENT_RANGE(LifeCycleIdx);
    solve part_idx before dai_addr;
  }
  constraint dai_wr_blank_addr_c {
    dai_addr % 4 == 0;
    if ((PartInfoSecret >> part_idx) & 1) dai_addr % 8 == 0;
  }
  constraint num_trans_c {
    if (cfg_smoke_test) {
      num_trans == 1;
      num_dai_op inside {[1:2]};
    } else {
      num_trans  inside {[1:2]};
      num_dai_op inside {[1:50]};
    }
  }
  constraint regwens_c {
    check_regwen_val         dist {0 :/ 1, 1 :/ 9};
    check_trigger_regwen_val dist {0 :/ 1, 1 :/ 9};
  }
  constraint check_timeout_val_c {
    check_timeout_val inside {0, [100_000:'1]};
  }
  constraint ecc_otp_err_c {ecc_otp_err == OtpNoEccErr;}
  constraint ecc_chk_err_c {ecc_chk_err == OtpNoEccErr;}
  constraint apply_reset_during_pwr_init_cycles_c {
    apply_reset_during_pwr_init_cycles dist {
        [1:5]       :/ 4,
        [6:2000]    :/ 4,
        [2001:4000] :/ 2};
  }
endclass
module top;
  initial begin
    otp_ctrl_smoke_vseq t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d %0d %h %0d %0d %0d %h %b%b", i, t.part_idx, t.dai_addr, t.num_trans,
               t.num_dai_op, t.apply_reset_during_pwr_init_cycles, t.check_timeout_val,
               t.check_regwen_val, t.check_trigger_regwen_val);
    end
    $finish;
  end
endmodule
