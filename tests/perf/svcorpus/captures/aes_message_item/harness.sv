// OpenTitan hw/ip/aes/dv/env/aes_message_item.sv: all constraint blocks; knobs as
// aes_base_vseq::aes_message_init() copies them from cfg under aes_config_error_test.
package p;
  typedef enum logic [1:0] {
    AES_ENC = 2'b01,
    AES_DEC = 2'b10
  } aes_op_e;
  typedef enum logic [5:0] {
    AES_ECB  = 6'b00_0001,
    AES_CBC  = 6'b00_0010,
    AES_CFB  = 6'b00_0100,
    AES_OFB  = 6'b00_1000,
    AES_CTR  = 6'b01_0000,
    AES_NONE = 6'b10_0000
  } aes_mode_e;
  typedef enum logic [2:0] {
    AES_128 = 3'b001,
    AES_192 = 3'b010,
    AES_256 = 3'b100
  } key_len_e;
  typedef enum logic [2:0] {
    PER_1  = 3'b001,
    PER_64 = 3'b010,
    PER_8K = 3'b100
  } prs_rate_e;
  typedef struct packed {
    bit          lc_esc;
    bit          reset;
    bit          mal_inject;
    bit          cfg;
  } error_types_t;
  typedef struct packed {
    bit          key_len;
    bit          mode;
    bit          rsd_rate;
    bit          op;
  } cfg_error_type_t;
endpackage
import p::*;
class aes_message_item;
  // knobs: aes_config_error_test values (cfg defaults where that test leaves them)
  int               message_len_max       = 32;
  int               message_len_min       = 16;
  int               config_error_pct      = 75;
  cfg_error_type_t  config_error_type_en  = '{key_len: 1'b1, mode: 1'b1, rsd_rate: 1'b1, op: 1'b1};
  error_types_t     error_types           = 4'b0001;
  int               manual_operation_pct  = 5;
  bit               fixed_key_en          = 0;
  bit               fixed_keylen_en       = 0;
  bit               fixed_operation_en    = 0;
  bit               fixed_iv_en           = 0;
  int               sideload_pct          = 0;
  rand bit          sideload_en           = 0;

  bit [7:0] [31:0]    fixed_key [2]    = '{256'h0000111122223333444455556666777788889999AAAABBBBCCCCDDDDEEEEFFFF, 256'h0};
  bit [2:0]           fixed_keylen     = 3'b001;
  bit [1:0]           fixed_operation  = AES_ENC;
  bit [3:0] [31:0]    fixed_iv         = 128'h00000000000000000000000000000000;

  int    ecb_weight           = 10;
  int    cbc_weight           = 10;
  int    cfb_weight           = 10;
  int    ofb_weight           = 10;
  int    ctr_weight           = 10;

  int    key_128b_weight      = 10;
  int    key_192b_weight      = 10;
  int    key_256b_weight      = 10;

  int    per1_weight          = 60;
  int    per64_weight         = 30;
  int    per8k_weight         = 10;

  rand int               message_length;
  rand aes_mode_e        aes_mode = AES_NONE;
  rand bit [1:0]         aes_operation;
  rand bit [2:0]         aes_keylen;
  // `rand` dropped: >64-bit rand vars (unconstrained here: fixed_key_en = fixed_iv_en = 0)
  bit [7:0][31:0]   aes_key [2];
  bit [3:0][31:0]   aes_iv;
  rand bit               has_config_error;
  rand cfg_error_type_t  cfg_error_type;
  rand bit               manual_operation;
  rand bit [2:0]         reseed_rate;

  constraint data_c { message_length inside { [message_len_min:message_len_max] };}

  constraint has_config_error_c {
    if (error_types.cfg) {
      has_config_error dist { 0 :/ (100 - config_error_pct),
                              1 :/ config_error_pct};
    } else {
      has_config_error == 0;
    }
  }

  constraint config_error_type_c {
    solve has_config_error before cfg_error_type;
    if (has_config_error) {
      cfg_error_type inside {[1:15]};
      config_error_type_en.op       == 0 -> cfg_error_type.op       == 0;
      config_error_type_en.rsd_rate == 0 -> cfg_error_type.rsd_rate == 0;
      config_error_type_en.mode     == 0 -> cfg_error_type.mode     == 0;
      config_error_type_en.key_len  == 0 -> cfg_error_type.key_len  == 0;
    } else {
      cfg_error_type == '0;
    }
  }

  constraint keylen_c {
    solve has_config_error before aes_keylen;
    solve cfg_error_type before aes_keylen;
    if (!(has_config_error && cfg_error_type.key_len) ) {
      aes_keylen inside { AES_128, AES_192, AES_256 };
      aes_keylen dist   { AES_128 := key_128b_weight,
                          AES_192 := key_192b_weight,
                          AES_256 := key_256b_weight };
    } else {
      !(aes_keylen inside { AES_128, AES_192, AES_256 });
    }
    if (fixed_keylen_en) {
      aes_keylen == fixed_keylen
    };
  }

  constraint mode_c {
    solve has_config_error before aes_mode;
    solve cfg_error_type before aes_mode;
    if (!(has_config_error && cfg_error_type.mode)) {
      aes_mode dist   { AES_ECB := ecb_weight,
                        AES_CBC := cbc_weight,
                        AES_CFB := cfb_weight,
                        AES_OFB := ofb_weight,
                        AES_CTR := ctr_weight};
    } else {
      aes_mode == AES_NONE;
    }
  }

  constraint rsd_rate_c {
    solve has_config_error before reseed_rate;
    solve cfg_error_type before reseed_rate;
    if (!(has_config_error && cfg_error_type.rsd_rate) ) {
      reseed_rate inside { PER_1, PER_64, PER_8K };
      reseed_rate dist   { PER_1  :/ per1_weight,
                           PER_64 :/ per64_weight,
                           PER_8K :/ per8k_weight };
    } else {
      !( reseed_rate  inside { PER_1, PER_64, PER_8K });
    }
  }

  constraint operation_c {
    solve has_config_error before aes_operation;
    solve cfg_error_type before aes_operation;
    if (!(has_config_error && cfg_error_type.op) ) {
      aes_operation inside { AES_ENC, AES_DEC };
      aes_operation dist   { AES_ENC :/ 1,
                             AES_DEC :/ 1 };
    } else {
      !( aes_operation inside { AES_ENC, AES_DEC });
    }
    if (fixed_operation_en) {
      aes_operation == fixed_operation
    };
  }

  constraint key_c {
    if (fixed_key_en) {
      aes_key[0] == fixed_key[0],
      aes_key[1] == fixed_key[1]
    };
  }

  constraint iv_c {
    if (fixed_iv_en) {
      aes_iv == fixed_iv
    };
  }

  constraint sideload_c {
    sideload_en dist{ 0:/(100-sideload_pct),
                      1:/sideload_pct};
  }

  constraint manual_operation_c {
    solve sideload_en before manual_operation;
    if (!sideload_en) {
      manual_operation dist { 0:/ (100 - manual_operation_pct),
                              1:/ manual_operation_pct};
    } else {
      manual_operation == 0 ;
    }
  }
endclass
module top;
  initial begin
    aes_message_item t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d len=%0d mode=%h op=%0d kl=%0d cfgerr=%b %b rr=%0d man=%b sl=%b", i, t.message_length,
               t.aes_mode, t.aes_operation, t.aes_keylen, t.has_config_error, t.cfg_error_type,
               t.reseed_rate, t.manual_operation, t.sideload_en);
    end
    $finish;
  end
endmodule
