// OpenTitan hw/ip_templates/flash_ctrl/dv/env/seq_lib/flash_ctrl_otf_base_vseq.sv:
// rand fields + constraint blocks of a flash op (earlgrey flash_ctrl parameters; UVM stripped;
// cfg / cfg.seq_cfg knobs are their defaults, as constants of stand-in objects)
package p;
  typedef int unsigned uint;
  parameter int TL_AW = 32;
  parameter int TL_DW = 32;
  // flash_ctrl_top_specific_pkg (earlgrey)
  parameter int unsigned NumBanks     = 2;
  parameter int unsigned PagesPerBank = 256;
  parameter int InfoTypes             = 3;
  parameter int DataWidth             = 64;
  parameter int InfoTypeSize [InfoTypes] = '{10, 1, 2};
  parameter int WordsPerPage          = 256;
  parameter int MpRegions             = 8;
  parameter int BusAddrByteW          = 20;
  typedef enum logic [1:0] {
    FlashOpRead = 2'h0, FlashOpProgram = 2'h1, FlashOpErase = 2'h2, FlashOpInvalid = 2'h3
  } flash_op_e;
  typedef enum logic { FlashErasePage = 0, FlashEraseBank = 1 } flash_erase_e;
  typedef enum logic [3:0] { MuBi4True = 4'h6, MuBi4False = 4'h9 } mubi4_t;
  // flash_ctrl_env_pkg
  parameter uint FlashNumPages  = NumBanks * PagesPerBank;
  parameter uint FlashSizeBytes = FlashNumPages * WordsPerPage * DataWidth / 8;
  parameter uint BytesPerPage   = FlashSizeBytes / FlashNumPages;
  parameter uint InfoTypeBytes[InfoTypes] = '{
      InfoTypeSize[0] * BytesPerPage,
      InfoTypeSize[1] * BytesPerPage,
      InfoTypeSize[2] * BytesPerPage
  };
  parameter uint FlashBankBytesPerWord = DataWidth / 8;
  localparam int unsigned CtrlTransMin = 1;
  localparam int unsigned CtrlTransMax = 32;
  typedef enum bit [1:0] { OTFCfgTrue, OTFCfgFalse, OTFCfgRand } otf_cfg_mode_e;
  typedef enum logic [InfoTypes:0] {
    FlashPartData = 0, FlashPartInfo = 1, FlashPartInfo1 = 2, FlashPartInfo2 = 4
  } flash_dv_part_e;
  typedef enum bit { FlashProgSelNormal = 0, FlashProgSelRepair = 1 } flash_prog_sel_e;
  typedef enum { TgtRd = 0, TgtDr = 1, TgtWr = 2, TgtEr = 3, NumTgt = 4 } flash_tgt_prefix_e;
  typedef enum { FlashEccDisabled, FlashEccEnabled, FlashSerrTestMode, FlashDerrTestMode,
                 FlashIerrTestMode } ecc_mode_e;
  typedef struct packed {
    mubi4_t en;
    mubi4_t read_en;
    mubi4_t program_en;
    mubi4_t erase_en;
    mubi4_t scramble_en;
    mubi4_t ecc_en;
    mubi4_t he_en;
    uint    num_pages;
    uint    start_page;
  } flash_mp_region_cfg_t;
  typedef bit [TL_AW-1:0] addr_t;
  typedef struct packed {
    flash_dv_part_e  partition;
    flash_erase_e    erase_type;
    flash_op_e       op;
    flash_prog_sel_e prog_sel;
    uint             num_words;
    addr_t           addr;
    bit [BusAddrByteW-2:0] otf_addr;
  } flash_op_t;
endpackage
import p::*;
class flash_ctrl_seq_cfg;
  bit avoid_ro_partitions = 0;
  bit trigger_prog_res_fault = 0;
  flash_tgt_prefix_e ecc_err_target = TgtRd;
  bit op_readonly_on_info_partition = 0;
  bit op_readonly_on_info1_partition = 1;
  bit addr_flash_word_aligned = 1'b0;
endclass
class flash_ctrl_env_cfg;
  flash_ctrl_seq_cfg seq_cfg = new;
  ecc_mode_e ecc_mode = FlashEccDisabled;
  bit [1:0] tgt_pre[flash_dv_part_e][NumTgt];
  bit en_always_read = 0;
  bit en_always_any = 0;
  function new();
    foreach (tgt_pre[FlashPartData][i]) tgt_pre[FlashPartData][i] = i;
    foreach (tgt_pre[FlashPartInfo][i]) tgt_pre[FlashPartInfo][i] = i;
    foreach (tgt_pre[FlashPartInfo1][i]) tgt_pre[FlashPartInfo1][i] = i;
    foreach (tgt_pre[FlashPartInfo2][i]) tgt_pre[FlashPartInfo2][i] = i;
  endfunction
endclass
class flash_ctrl_otf_base_vseq;
  flash_ctrl_env_cfg cfg = new;
  rand int  ctrl_data_num;
  rand int  ctrl_info_num;
  rand int  ctrl_num;
  rand bit  is_addr_odd;
  rand int  fractions;
  rand flash_op_t rand_op;
  rand bit [2:0] allow_spec_info_acc;
  rand bit       all_entry_en;
  rand otf_cfg_mode_e scr_ecc_cfg;
  rand flash_mp_region_cfg_t rand_regions[MpRegions];

  constraint all_ent_c {
    solve all_entry_en before rand_regions;
    if (cfg.en_always_any) all_entry_en == 1;
    else all_entry_en dist { 1 := 1, 0 := 4};
  }
  constraint scr_ecc_c {
    scr_ecc_cfg dist { OTFCfgRand := 5, OTFCfgTrue := 4};
  }
  constraint rand_regions_c {
    foreach (rand_regions[i]) {
      if (all_entry_en) rand_regions[i].en == MuBi4True;
      rand_regions[i].start_page dist {
        0                       := 1,
        [1 : FlashNumPages - 2] :/ 8,
        FlashNumPages - 1       := 1
      };
      rand_regions[i].num_pages inside {[1 : FlashNumPages - rand_regions[i].start_page]};
      rand_regions[i].num_pages <= 32;
      rand_regions[i].scramble_en dist { MuBi4True := 4, MuBi4False := 1};
      rand_regions[i].ecc_en dist { MuBi4True := 4, MuBi4False := 1};
    }
  }
  constraint ctrl_data_num_c {
    ctrl_data_num dist { CtrlTransMin := 2, [2:31] :/ 1, CtrlTransMax := 2};
  }
  constraint fractions_c {
    if (cfg.seq_cfg.trigger_prog_res_fault) fractions inside {[17:32]};
    else fractions dist { [1:4] := 4, [5:15] := 1, 16 := 1};
    cfg.seq_cfg.addr_flash_word_aligned -> fractions[0] == 1'b0;
  }
  constraint ctrl_info_num_c {
    solve rand_op before ctrl_info_num;
    // (V5.052) a (parameter or member) array indexed by a rand expression is emitted as a
    // bad select (`(select #x00000007 i)`): InfoTypeSize[rand_op.partition >> 1] as a ?:
    // over the index, 0 out of bounds as SV reads it.
    // ctrl_info_num inside {[1 : InfoTypeSize[rand_op.partition >> 1]]};
    ctrl_info_num inside {[1 : (rand_op.partition >> 1) == 0 ? InfoTypeSize[0] :
                               (rand_op.partition >> 1) == 1 ? InfoTypeSize[1] :
                               (rand_op.partition >> 1) == 2 ? InfoTypeSize[2] : 0]};
    if (cfg.ecc_mode > FlashEccEnabled) ctrl_info_num * fractions <= 128;
  }
  constraint ctrl_num_c {
    solve ctrl_data_num, ctrl_info_num, rand_op before ctrl_num;
    if (rand_op.partition == FlashPartData) ctrl_num == ctrl_data_num;
    else ctrl_num == ctrl_info_num;
  }
  constraint rand_op_c {
    solve fractions before rand_op.addr;
    // (V5.052) Verilator reads a member of the packed struct as the whole struct, so these
    // three become `rand_op before rand_op` (circular, randomize() fails): dropped here.
    // solve rand_op.partition before rand_op.prog_sel, rand_op.addr;
    // solve rand_op.addr before rand_op.otf_addr;
    // solve rand_op.addr before rand_op.num_words;

    if (cfg.seq_cfg.op_readonly_on_info_partition) {
      if (cfg.seq_cfg.avoid_ro_partitions) {
        rand_op.partition != FlashPartInfo;
      } else {
        rand_op.partition == FlashPartInfo ->
          rand_op.op == FlashOpRead;
      }
    }
    if (cfg.seq_cfg.op_readonly_on_info1_partition) {
      if (cfg.seq_cfg.avoid_ro_partitions) {
        rand_op.partition != FlashPartInfo1;
      } else {
        rand_op.partition == FlashPartInfo1 ->
          rand_op.op == FlashOpRead;
      }
    }
    rand_op.partition dist {
      FlashPartData := 6,
      FlashPartInfo := 1,
      FlashPartInfo1 := 1,
      FlashPartInfo2 := 1
    };
    rand_op.addr[TL_AW-1:BusAddrByteW] == 'h0;
    rand_op.addr[1:0] == 'h0;
    cfg.seq_cfg.addr_flash_word_aligned -> rand_op.addr[2] == 1'b0;
    if (fractions == 16) rand_op.addr[2] == 0;
    if (rand_op.partition != FlashPartData) {
      rand_op.addr inside
        {[0:((rand_op.partition>>1) == 0 ? InfoTypeBytes[0] :
             (rand_op.partition>>1) == 1 ? InfoTypeBytes[1] :
             (rand_op.partition>>1) == 2 ? InfoTypeBytes[2] : 0)
            -(FlashBankBytesPerWord*fractions)]};
      // (as above) was {[0:InfoTypeBytes[rand_op.partition>>1]-(FlashBankBytesPerWord*fractions)]}
      rand_op.prog_sel == 1;
    } else {
      rand_op.prog_sel == 0;
    }
    // (V5.052) the associative array cfg.tgt_pre indexed by a rand enum is emitted as a
    // malformed select even under the constant-false guard (ecc_mode is FlashEccDisabled):
    // block dropped here, kept in the case.
    // if (cfg.ecc_mode > FlashEccEnabled) {
    //   if (rand_op.partition == FlashPartData) {
    //     rand_op.addr[18:17] == cfg.tgt_pre[rand_op.partition][cfg.seq_cfg.ecc_err_target];
    //   } else {
    //     rand_op.addr[10:9] == cfg.tgt_pre[rand_op.partition][cfg.seq_cfg.ecc_err_target];
    //   }
    // }
    rand_op.otf_addr == rand_op.addr[BusAddrByteW-2:0];
    rand_op.num_words == fractions;
  }
  constraint special_info_acc_c {
    allow_spec_info_acc dist { 3'h7 := 1, 3'h0 := 1, [1:6] :/ 2};
  }
endclass
module top;
  initial begin
    flash_ctrl_otf_base_vseq s = new;
    for (int i = 0; i < 20; i++) begin
      if (!s.randomize()) $display("FAIL");
      $display("%0d dn=%0d in=%0d n=%0d odd=%b fr=%0d part=%0d er=%0d op=%0d ps=%0d nw=%0d addr=%h otf=%h acc=%0d all=%b scr=%0d r0=%0d+%0d %h %h r7=%0d+%0d",
        i, s.ctrl_data_num, s.ctrl_info_num, s.ctrl_num, s.is_addr_odd, s.fractions,
        s.rand_op.partition, s.rand_op.erase_type, s.rand_op.op, s.rand_op.prog_sel,
        s.rand_op.num_words, s.rand_op.addr, s.rand_op.otf_addr, s.allow_spec_info_acc,
        s.all_entry_en, s.scr_ecc_cfg, s.rand_regions[0].start_page, s.rand_regions[0].num_pages,
        s.rand_regions[0].en, s.rand_regions[0].scramble_en,
        s.rand_regions[7].start_page, s.rand_regions[7].num_pages);
    end
    $finish;
  end
endmodule
