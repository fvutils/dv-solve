"""OpenTitan aes: a message's length, mode, key length, reseed rate and errors.

    rand bit          sideload_en           = 0;
    rand int               message_length;
    rand aes_mode_e        aes_mode = AES_NONE;  // ECB=1 CBC=2 CFB=4 OFB=8 CTR=16 NONE=32
    rand bit [1:0]         aes_operation;        // AES_ENC = 1, AES_DEC = 2
    rand bit [2:0]         aes_keylen;           // AES_128/192/256 = 1/2/4
    rand bit [7:0][31:0]   aes_key [2];
    rand bit [3:0][31:0]   aes_iv;
    rand bit               has_config_error;
    rand cfg_error_type_t  cfg_error_type;       // packed {key_len, mode, rsd_rate, op}
    rand bit               manual_operation;
    rand bit [2:0]         reseed_rate;          // PER_1/64/8K = 1/2/4

    constraint data_c { message_length inside { [message_len_min:message_len_max] };}
    constraint has_config_error_c {
      if (error_types.cfg) {
        has_config_error dist { 0 :/ (100 - config_error_pct), 1 :/ config_error_pct};
      } else { has_config_error == 0; }
    }
    constraint config_error_type_c {
      solve has_config_error before cfg_error_type;
      if (has_config_error) {
        cfg_error_type inside {[1:15]};
        config_error_type_en.op       == 0 -> cfg_error_type.op       == 0;
        config_error_type_en.rsd_rate == 0 -> cfg_error_type.rsd_rate == 0;
        config_error_type_en.mode     == 0 -> cfg_error_type.mode     == 0;
        config_error_type_en.key_len  == 0 -> cfg_error_type.key_len  == 0;
      } else { cfg_error_type == '0; }
    }
    constraint keylen_c {
      solve has_config_error before aes_keylen;
      solve cfg_error_type before aes_keylen;
      if (!(has_config_error && cfg_error_type.key_len) ) {
        aes_keylen inside { AES_128, AES_192, AES_256 };
        aes_keylen dist   { AES_128 := key_128b_weight, AES_192 := key_192b_weight,
                            AES_256 := key_256b_weight };
      } else { !(aes_keylen inside { AES_128, AES_192, AES_256 }); }
      if (fixed_keylen_en) { aes_keylen == fixed_keylen };
    }
    constraint mode_c {        // (solve has_config_error, cfg_error_type before aes_mode)
      if (!(has_config_error && cfg_error_type.mode)) {
        aes_mode dist { AES_ECB := ecb_weight, AES_CBC := cbc_weight, AES_CFB := cfb_weight,
                        AES_OFB := ofb_weight, AES_CTR := ctr_weight};
      } else { aes_mode == AES_NONE; }
    }
    constraint rsd_rate_c {    // (solve has_config_error, cfg_error_type before reseed_rate)
      if (!(has_config_error && cfg_error_type.rsd_rate) ) {
        reseed_rate inside { PER_1, PER_64, PER_8K };
        reseed_rate dist   { PER_1 :/ per1_weight, PER_64 :/ per64_weight,
                             PER_8K :/ per8k_weight };
      } else { !( reseed_rate  inside { PER_1, PER_64, PER_8K }); }
    }
    constraint operation_c {   // (solve has_config_error, cfg_error_type before aes_operation)
      if (!(has_config_error && cfg_error_type.op) ) {
        aes_operation inside { AES_ENC, AES_DEC };
        aes_operation dist   { AES_ENC :/ 1, AES_DEC :/ 1 };
      } else { !( aes_operation inside { AES_ENC, AES_DEC }); }
      if (fixed_operation_en) { aes_operation == fixed_operation };
    }
    constraint key_c {
      if (fixed_key_en) { aes_key[0] == fixed_key[0], aes_key[1] == fixed_key[1] };
    }
    constraint iv_c { if (fixed_iv_en) { aes_iv == fixed_iv }; }
    constraint sideload_c { sideload_en dist{ 0:/(100-sideload_pct), 1:/sideload_pct}; }
    constraint manual_operation_c {
      solve sideload_en before manual_operation;
      if (!sideload_en) {
        manual_operation dist { 0:/ (100 - manual_operation_pct), 1:/ manual_operation_pct};
      } else { manual_operation == 0 ; }
    }

Knobs (non-rand state): the values aes_base_vseq::aes_message_init() copies
from cfg under aes_config_error_test (the item's own defaults disable every
error path): error_types = 4'b0001 (cfg on), config_error_pct = 75,
config_error_type_en = all ones, message_len 16..32, manual_operation_pct = 5,
all mode/key weights 10; cfg defaults where that test leaves them:
per1/64/8k = 60/30/10, sideload_pct = 0, fixed_*_en = 0, fixed_keylen =
3'b001, fixed_operation = AES_ENC.

Changed for the corpus: aes_key and aes_iv lose `rand` (they are 256- and
128-bit, beyond the builder's 64-bit variables and constants, and no
constraint reads them with fixed_key_en = fixed_iv_en = 0); key_c and iv_c
then read only state and the case writes them folded: key_c's body is
`{aes_key[0] == fixed_key[0], aes_key[1] == fixed_key[1]}` = {0, 1} (the
key starts at 0 and fixed_key[1] is 0), iv_c's is 1, both under a 0 guard.
Note `{a == b, c == d}` is a concatenation, so key_c (when enabled) only
needs ONE of the two key halves to match; Verilator parses it the same way.

`dist` inside if/else: the membership is part of its branch; DIST records
the weights unconditionally (the format has no condition). sideload_en's
`1 :/ 0` has weight 0 (a member of the dist, never drawn), so the
membership is {0, 1} as Verilator states it.
"""
from dv_solve.problem import BIN_AND, BIN_EQ, BIN_NEQ, UN_NOT

from tests.perf.svcorpus.case import all_of, implies, inside_dist

NAME = "aes_message_item"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/ip/aes/dv/env/aes_message_item.sv", "lines": "13-235"}
ABOUT = "message length, error-type knobs gating mode/keylen/reseed/op sparse one-hot sets and dists"

VARS = [("sideload_en", 1, False), ("message_length", 32, True), ("aes_mode", 6, False),
        ("aes_operation", 2, False), ("aes_keylen", 3, False), ("has_config_error", 1, False),
        ("cfg_error_type", 4, False), ("manual_operation", 1, False), ("reseed_rate", 3, False)]

AES_ECB, AES_CBC, AES_CFB, AES_OFB, AES_CTR, AES_NONE = 1, 2, 4, 8, 16, 32
AES_128, AES_192, AES_256 = 1, 2, 4
PER_1, PER_64, PER_8K = 1, 2, 4
AES_ENC, AES_DEC = 1, 2
CONFIG_ERROR_PCT, MANUAL_OPERATION_PCT, SIDELOAD_PCT = 75, 5, 0

DIST = {
    "has_config_error": [(0, 0, 100 - CONFIG_ERROR_PCT, False), (1, 1, CONFIG_ERROR_PCT, False)],
    "aes_keylen": [(AES_128, AES_128, 10, True), (AES_192, AES_192, 10, True),
                   (AES_256, AES_256, 10, True)],
    "aes_mode": [(k, k, 10, True) for k in (AES_ECB, AES_CBC, AES_CFB, AES_OFB, AES_CTR)],
    "reseed_rate": [(PER_1, PER_1, 60, False), (PER_64, PER_64, 30, False),
                    (PER_8K, PER_8K, 10, False)],
    "aes_operation": [(AES_ENC, AES_ENC, 1, False), (AES_DEC, AES_DEC, 1, False)],
    "sideload_en": [(0, 0, 100 - SIDELOAD_PCT, False), (1, 1, SIDELOAD_PCT, False)],
    "manual_operation": [(0, 0, 100 - MANUAL_OPERATION_PCT, False),
                         (1, 1, MANUAL_OPERATION_PCT, False)],
}
BOX = {"message_length": (16, 32)}          # data_c; equiv.py proves it
# The dists of keylen_c, mode_c, rsd_rate_c, operation_c are in the branch
# without a config error on that field (cfg_error_type bit 3, 2, 1, 0);
# manual_operation's, in the branch without sideload.
_ok = lambda bit: lambda m: not (m["has_config_error"] and (m["cfg_error_type"] >> bit) & 1)  # noqa: E731
DIST_WHEN = {"aes_keylen": _ok(3), "aes_mode": _ok(2), "reseed_rate": _ok(1),
             "aes_operation": _ok(0), "manual_operation": lambda m: not m["sideload_en"]}
SOLVE_BEFORE = [(("has_config_error",), ("cfg_error_type",)),
                (("has_config_error",), ("aes_keylen",)), (("cfg_error_type",), ("aes_keylen",)),
                (("has_config_error",), ("aes_mode",)), (("cfg_error_type",), ("aes_mode",)),
                (("has_config_error",), ("reseed_rate",)), (("cfg_error_type",), ("reseed_rate",)),
                (("has_config_error",), ("aes_operation",)),
                (("cfg_error_type",), ("aes_operation",)),
                (("sideload_en",), ("manual_operation",))]


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    int_ = lambda k: b.expr_const(k, is_signed=True, width=32)   # noqa: E731
    c = lambda k, w: b.expr_const(k, width=w)              # noqa: E731
    eq = lambda x, y: b.expr_binary(BIN_EQ, x, y)          # noqa: E731
    not_ = lambda x: b.expr_unary(UN_NOT, x)               # noqa: E731
    ite = b.expr_ite
    hce, cet = var("has_config_error"), var("cfg_error_type")
    field = lambda bit: b.expr_extract(cet, bit, bit)      # noqa: E731  {key_len,mode,rsd_rate,op}
    KEY_LEN, MODE, RSD_RATE, OP = 3, 2, 1, 0
    no_err = lambda f: not_(b.expr_binary(BIN_AND, hce, field(f)))   # noqa: E731

    # rand aes_mode_e: one of its named values
    b.add_constraint(b.expr_in_set(var("aes_mode"), [c(k, 6) for k in
                                   (AES_ECB, AES_CBC, AES_CFB, AES_OFB, AES_CTR, AES_NONE)]))
    # data_c
    b.add_constraint(b.expr_in_range(var("message_length"), int_(16), int_(32)))
    # has_config_error_c: error_types.cfg = 1
    b.add_constraint(ite(c(1, 1), inside_dist(b, hce, DIST["has_config_error"]),
                         eq(hce, int_(0))))
    # config_error_type_c: config_error_type_en = all ones
    en = {f: c(1, 1) for f in (KEY_LEN, MODE, RSD_RATE, OP)}
    b.add_constraint(ite(hce,
                         all_of(b, b.expr_in_range(cet, int_(1), int_(15)),
                                *[implies(b, eq(en[f], int_(0)), eq(field(f), int_(0)))
                                  for f in (OP, RSD_RATE, MODE, KEY_LEN)]),
                         eq(cet, c(0, 4))))
    # keylen_c
    kl = var("aes_keylen")
    keylens = lambda: b.expr_in_set(kl, [c(k, 3) for k in (AES_128, AES_192, AES_256)])  # noqa: E731
    b.add_constraint(ite(no_err(KEY_LEN),
                         all_of(b, keylens(), inside_dist(b, kl, DIST["aes_keylen"], width=3)),
                         not_(keylens())))
    b.add_constraint(implies(b, c(0, 1), eq(kl, c(0b001, 3))))     # fixed_keylen_en = 0
    # mode_c
    mode = var("aes_mode")
    b.add_constraint(ite(no_err(MODE), inside_dist(b, mode, DIST["aes_mode"], width=6),
                         eq(mode, c(AES_NONE, 6))))
    # rsd_rate_c
    rr = var("reseed_rate")
    rates = lambda: b.expr_in_set(rr, [c(k, 3) for k in (PER_1, PER_64, PER_8K)])   # noqa: E731
    b.add_constraint(ite(no_err(RSD_RATE),
                         all_of(b, rates(), inside_dist(b, rr, DIST["reseed_rate"], width=3)),
                         not_(rates())))
    # operation_c
    op = var("aes_operation")
    ops = lambda: b.expr_in_set(op, [c(AES_ENC, 2), c(AES_DEC, 2)])   # noqa: E731
    b.add_constraint(ite(no_err(OP),
                         all_of(b, ops(), inside_dist(b, op, DIST["aes_operation"], width=2)),
                         not_(ops())))
    b.add_constraint(implies(b, c(0, 1), eq(op, c(AES_ENC, 2))))   # fixed_operation_en = 0
    # key_c, iv_c: state only (see the docstring), guards fixed_key_en = fixed_iv_en = 0
    b.add_constraint(implies(b, c(0, 1),
                             b.expr_binary(BIN_NEQ, b.expr_concat(c(0, 1), c(1, 1), 1), c(0, 2))))
    b.add_constraint(implies(b, c(0, 1), c(1, 1)))
    # sideload_c
    sl = var("sideload_en")
    b.add_constraint(inside_dist(b, sl, DIST["sideload_en"]))
    # manual_operation_c
    mo = var("manual_operation")
    b.add_constraint(ite(not_(sl), inside_dist(b, mo, DIST["manual_operation"]),
                         eq(mo, int_(0))))
