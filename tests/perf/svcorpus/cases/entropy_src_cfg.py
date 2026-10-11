"""OpenTitan entropy_src: the env config's randomized test knobs (error/FIFO/counter targets).

    rand int unsigned sim_duration_ms;
    rand logic [7:0] otp_en_es_fw_read, otp_en_es_fw_over;
    rand bit spurious_inject_entropy;
    rand fatal_err_e which_fatal_err;  rand err_code_e which_err_code;
    rand which_fifo_e which_fifo;      rand which_fifo_err_e which_fifo_err;
    rand ht_fail_e which_ht_fail;      rand cntr_e which_cntr;
    rand which_ht_e which_ht;          rand state_e which_ht_state;
    rand uint which_cntr_replicate, which_bin;
    rand bit induce_targeted_transition;  rand mubi4_t es_route_sw;
    rand int fw_ov_rd_cnt;
    constraint sim_duration_ms_c { 7 <= sim_duration_ms && sim_duration_ms <= 20; }
    constraint which_ht_state_c { which_ht_state dist { BootHTRunning :/ 25,
      BootPhaseDone :/ 25, StartupFail1 :/ 25, ContHTRunning :/ 25 }; }
    constraint otp_en_es_fw_read_c {
      `DV_MUBI8_DIST(otp_en_es_fw_read, otp_en_es_fw_read_pct,
                     100 - otp_en_es_fw_read_pct - otp_en_es_fw_read_inval_pct,
                     otp_en_es_fw_read_inval_pct) }
    constraint otp_en_es_fw_over_c { ... the same for otp_en_es_fw_over ... }
    constraint spurious_inject_entropy_c { spurious_inject_entropy dist {
      1 :/ spurious_inject_entropy_pct, 0 :/ (100 - spurious_inject_entropy_pct) }; }
    constraint which_err_code_c { which_err_code dist { sfifo_esrng_err :/ 2,
      sfifo_distr_err :/ 2, sfifo_observe_err :/ 2, sfifo_esfinal_err :/ 2,
      es_ack_sm_err :/ 2, es_main_sm_err :/ 2, es_cntr_err :/ 60, fifo_read_err :/ 4,
      fifo_state_err :/ 4, fifo_cntr_err :/ 4}; }
    constraint which_cntr_replicate_c { which_cntr_replicate inside {[0:RNG_BUS_WIDTH-1]}; }
    int num_bins = 2**RNG_BUS_WIDTH;
    constraint which_bin_c { which_bin inside {[0:num_bins-1]}; }
    constraint which_cntr_c { which_cntr dist { window_cntr :/ 1, repcnt_ht_cntr :/ 4,
      repcnts_ht_cntr :/ 1, adaptp_ht_cntr :/ 4, bucket_ht_cntr :/ 16, markov_ht_cntr :/ 4}; }
    constraint which_fifo_err_c {
      which_err_code inside {sfifo_esrng_err, sfifo_distr_err, sfifo_esfinal_err} ->
        which_fifo_err inside {read, state};
      which_err_code == fifo_read_err -> which_fifo_err == read;
      which_err_code inside {fifo_state_err, fifo_cntr_err} -> which_fifo_err == state; }
    constraint which_fifo_c {
      which_err_code == sfifo_observe_err -> which_fifo == sfifo_observe;
      which_err_code == sfifo_esrng_err -> which_fifo == sfifo_esrng;
      which_err_code == sfifo_distr_err -> which_fifo == sfifo_distr;
      which_err_code == sfifo_esfinal_err -> which_fifo == sfifo_esfinal;
      which_err_code == fifo_cntr_err -> which_fifo inside {sfifo_observe, sfifo_esrng,
                                                            sfifo_distr, sfifo_esfinal}; }
    constraint induce_targeted_transition_c { induce_targeted_transition dist {
      1 :/ induce_targeted_transition_pct, 0 :/ (100 - induce_targeted_transition_pct) }; }
    constraint es_route_sw_c { es_route_sw inside {MuBi4False, MuBi4True}; }
    constraint fw_ov_rd_cnt_c { fw_ov_rd_cnt dist { 1 :/ 40, [2:127] :/ 20, 128 :/ 40 }; }

`DV_MUBI8_DIST(V, T, F, O)` (cip_macros.svh) expands to
    if (MuBi8True > MuBi8False) { V dist { MuBi8True := T * (255 - 1),
      MuBi8False := F * (255 - 1), [0 : MuBi8False - 1] := O,
      [MuBi8False + 1 : MuBi8True - 1] := O, [MuBi8True + 1 : 255] := O }; }
    else { ... the same with True and False swapped ... }
with MuBi8True = 8'h96, MuBi8False = 8'h69 (MuBi4True = 4'h6, MuBi4False = 4'h9).
RNG_BUS_WIDTH = 4. The enums are `typedef enum int` (32-bit signed) except
state_e (entropy_src_main_sm_pkg, logic [8:0], 19 sparse encodings) and mubi4_t.

Non-rand knobs as constants: the pct knobs default to 0 (a uint), which
would make several dist weights 0; they are set as entropy_src_rng_test does:
otp_en_es_fw_{read,over}_pct = otp_en_es_fw_{read,over}_inval_pct = 33,
spurious_inject_entropy_pct = 50, induce_targeted_transition_pct = 75.
Inherited constraints (cip_base_env_cfg / dv_base_env_cfg) only constrain
their own fields, so they are left out, as are the rand agent-cfg handles.
which_fatal_err, which_ht_fail and which_ht appear in no constraint:
Verilator leaves them out of the solver query (it draws them itself among
the named values), so the case declares them with no membership constraint,
to match the capture; every other rand enum gets its named-value membership,
as Verilator emits it. Constant bound expressions (RNG_BUS_WIDTH-1,
num_bins-1, the macro's MuBi8False - 1 ...) are folded to literals of their
SV type, as elaboration does (the solver does not compile an `inside` range
whose bound is an unfolded constant expression; see the corpus report).
"""
from dv_solve.problem import BIN_EQ, BIN_GT, BIN_LTE, BIN_OR, UN_NOT

from tests.perf.svcorpus.case import all_of, implies

NAME = "entropy_src_cfg"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/ip/entropy_src/dv/env/entropy_src_env_cfg.sv", "lines": "131-232"}
ABOUT = "entropy_src test knobs: mubi8 OTP dists, weighted error/counter targets, error->FIFO implications"

VARS = [("sim_duration_ms", 32, False), ("otp_en_es_fw_read", 8, False),
        ("otp_en_es_fw_over", 8, False), ("spurious_inject_entropy", 1, False),
        ("which_fatal_err", 32, True), ("which_err_code", 32, True),
        ("which_fifo", 32, True), ("which_fifo_err", 32, True),
        ("which_ht_fail", 32, True), ("which_cntr", 32, True), ("which_ht", 32, True),
        ("which_ht_state", 9, False), ("which_cntr_replicate", 32, False),
        ("which_bin", 32, False), ("induce_targeted_transition", 1, False),
        ("es_route_sw", 4, False), ("fw_ov_rd_cnt", 32, True)]

RNG_BUS_WIDTH = 4
OTP_EN_ES_FW_READ_PCT, OTP_EN_ES_FW_READ_INVAL_PCT = 33, 33
OTP_EN_ES_FW_OVER_PCT, OTP_EN_ES_FW_OVER_INVAL_PCT = 33, 33
SPURIOUS_INJECT_ENTROPY_PCT = 50
INDUCE_TARGETED_TRANSITION_PCT = 75

MUBI8_TRUE, MUBI8_FALSE = 0x96, 0x69
MUBI4_TRUE, MUBI4_FALSE = 0x6, 0x9
# err_code_e
(SFIFO_ESRNG_ERR, SFIFO_DISTR_ERR, SFIFO_OBSERVE_ERR, SFIFO_ESFINAL_ERR, ES_ACK_SM_ERR,
 ES_MAIN_SM_ERR, ES_CNTR_ERR, FIFO_READ_ERR, FIFO_STATE_ERR, FIFO_CNTR_ERR) = range(10)
ERR_CODE_E = range(19)
WRITE, READ, STATE = 0, 1, 2                               # which_fifo_err_e
SFIFO_ESRNG, SFIFO_DISTR, SFIFO_OBSERVE, SFIFO_ESFINAL = 0, 1, 2, 3   # which_fifo_e
CNTR_E = range(6)        # window, repcnt_ht, repcnts_ht, adaptp_ht, bucket_ht, markov_ht
STATE_E = [0b011110101, 0b111010010, 0b101101110, 0b010001110, 0b000101100, 0b100000001,
           0b110100101, 0b000010111, 0b001000000, 0b110100010, 0b011000011, 0b001011001,
           0b100001111, 0b011111000, 0b010111111, 0b110011000, 0b111001101, 0b111111011,
           0b001110011]
BOOT_HT_RUNNING, BOOT_PHASE_DONE = 0b111010010, 0b010001110
STARTUP_FAIL1, CONT_HT_RUNNING = 0b000010111, 0b110100010


def _mubi8_dist(t, f, o):
    """DIST entries of DV_MUBI8_DIST (the True > False branch, the one taken)."""
    mx = (1 << 8) - 1
    return [(MUBI8_TRUE, MUBI8_TRUE, t * (mx - 1), True),
            (MUBI8_FALSE, MUBI8_FALSE, f * (mx - 1), True),
            (0, MUBI8_FALSE - 1, o, True),
            (MUBI8_FALSE + 1, MUBI8_TRUE - 1, o, True),
            (MUBI8_TRUE + 1, mx, o, True)]


def _bit_dist(pct):
    return [(1, 1, pct, False), (0, 0, 100 - pct, False)]


DIST = {
    "which_ht_state": [(x, x, 25, False) for x in (BOOT_HT_RUNNING, BOOT_PHASE_DONE,
                                                   STARTUP_FAIL1, CONT_HT_RUNNING)],
    "otp_en_es_fw_read": _mubi8_dist(OTP_EN_ES_FW_READ_PCT,
                                     100 - OTP_EN_ES_FW_READ_PCT - OTP_EN_ES_FW_READ_INVAL_PCT,
                                     OTP_EN_ES_FW_READ_INVAL_PCT),
    "otp_en_es_fw_over": _mubi8_dist(OTP_EN_ES_FW_OVER_PCT,
                                     100 - OTP_EN_ES_FW_OVER_PCT - OTP_EN_ES_FW_OVER_INVAL_PCT,
                                     OTP_EN_ES_FW_OVER_INVAL_PCT),
    "spurious_inject_entropy": _bit_dist(SPURIOUS_INJECT_ENTROPY_PCT),
    "which_err_code": [(x, x, w, False) for x, w in
                       ((SFIFO_ESRNG_ERR, 2), (SFIFO_DISTR_ERR, 2), (SFIFO_OBSERVE_ERR, 2),
                        (SFIFO_ESFINAL_ERR, 2), (ES_ACK_SM_ERR, 2), (ES_MAIN_SM_ERR, 2),
                        (ES_CNTR_ERR, 60), (FIFO_READ_ERR, 4), (FIFO_STATE_ERR, 4),
                        (FIFO_CNTR_ERR, 4))],
    "which_cntr": [(x, x, w, False) for x, w in zip(CNTR_E, (1, 4, 1, 4, 16, 4))],
    "induce_targeted_transition": _bit_dist(INDUCE_TARGETED_TRANSITION_PCT),
    "fw_ov_rd_cnt": [(1, 1, 40, False), (2, 127, 20, False), (128, 128, 40, False)],
}
SOLVE_BEFORE = []


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    i32 = lambda x: b.expr_const(x, is_signed=True, width=32)   # unsized literal / enum int item
    k = lambda x, w: b.expr_const(x, width=w)              # sized enum item  # noqa: E731
    eq = lambda a, c: b.expr_binary(BIN_EQ, a, c)          # noqa: E731
    ins = lambda x, ks: b.expr_in_set(x, ks)               # noqa: E731
    cond = lambda c, x: b.add_constraint(implies(b, c, x))  # noqa: E731

    def dist_in(x, items):
        """the hard part of `x dist {...}`: items are a constant term or a (lo, hi) pair"""
        sets = [it for it in items if not isinstance(it, tuple)]
        rngs = [it for it in items if isinstance(it, tuple)]
        parts = []
        if rngs:
            parts.append(b.expr_in_ranges(x, rngs))
        if sets:
            parts.append(ins(x, sets))
        acc = parts[0]
        for p_ in parts[1:]:
            acc = b.expr_binary(BIN_OR, acc, p_)
        return acc

    # rand enums take only their named values (those Verilator puts in the query)
    b.add_constraint(ins(var("which_err_code"), [i32(x) for x in ERR_CODE_E]))
    b.add_constraint(ins(var("which_fifo"), [i32(x) for x in range(4)]))
    b.add_constraint(ins(var("which_fifo_err"), [i32(x) for x in range(3)]))
    b.add_constraint(ins(var("which_cntr"), [i32(x) for x in CNTR_E]))
    b.add_constraint(ins(var("which_ht_state"), [k(x, 9) for x in STATE_E]))
    b.add_constraint(ins(var("es_route_sw"), [k(x, 4) for x in (MUBI4_TRUE, MUBI4_FALSE)]))

    # sim_duration_ms_c
    sd = var("sim_duration_ms")
    b.add_constraint(all_of(b, b.expr_binary(BIN_LTE, i32(7), sd),
                            b.expr_binary(BIN_LTE, sd, i32(20))))
    # which_ht_state_c
    b.add_constraint(dist_in(var("which_ht_state"),
                             [k(x, 9) for x in (BOOT_HT_RUNNING, BOOT_PHASE_DONE,
                                                STARTUP_FAIL1, CONT_HT_RUNNING)]))

    # otp_en_es_fw_{read,over}_c: `_DV_MUBI_DIST(V, MuBi8True, MuBi8False, (1 << 8) - 1, ...)
    def mubi8_dist(x):
        t, f = k(MUBI8_TRUE, 8), k(MUBI8_FALSE, 8)
        mx = (1 << 8) - 1
        # range bounds are constant expressions, folded as elaboration does:
        # MuBi8False - 1 etc. are 32-bit unsigned (enum op int), (1 << 8) - 1 an int
        u = lambda x: b.expr_const(x, width=32)        # noqa: E731
        first = dist_in(x, [t, f, (i32(0), u(MUBI8_FALSE - 1)),
                            (u(MUBI8_FALSE + 1), u(MUBI8_TRUE - 1)), (u(MUBI8_TRUE + 1), i32(mx))])
        second = dist_in(x, [t, f, (i32(0), u(MUBI8_TRUE - 1)),
                             (u(MUBI8_TRUE + 1), u(MUBI8_FALSE - 1)), (u(MUBI8_FALSE + 1), i32(mx))])
        gt = b.expr_binary(BIN_GT, t, f)
        b.add_constraint(implies(b, gt, first))
        b.add_constraint(implies(b, b.expr_unary(UN_NOT, gt), second))
    mubi8_dist(var("otp_en_es_fw_read"))
    mubi8_dist(var("otp_en_es_fw_over"))

    # spurious_inject_entropy_c, induce_targeted_transition_c
    b.add_constraint(dist_in(var("spurious_inject_entropy"), [i32(1), i32(0)]))
    b.add_constraint(dist_in(var("induce_targeted_transition"), [i32(1), i32(0)]))
    # which_err_code_c
    b.add_constraint(dist_in(var("which_err_code"), [i32(x) for x in range(10)]))
    # which_cntr_replicate_c, which_bin_c
    # (bounds folded: RNG_BUS_WIDTH-1 is a parameter expression, num_bins-1 reads a
    # non-rand int set to 2**RNG_BUS_WIDTH at construction)
    b.add_constraint(b.expr_in_range(var("which_cntr_replicate"), i32(0),
                                     i32(RNG_BUS_WIDTH - 1)))
    b.add_constraint(b.expr_in_range(var("which_bin"), i32(0), i32(2 ** RNG_BUS_WIDTH - 1)))
    # which_cntr_c
    b.add_constraint(dist_in(var("which_cntr"), [i32(x) for x in CNTR_E]))

    ec, fe, ff = var("which_err_code"), var("which_fifo_err"), var("which_fifo")
    # which_fifo_err_c
    cond(ins(ec, [i32(x) for x in (SFIFO_ESRNG_ERR, SFIFO_DISTR_ERR, SFIFO_ESFINAL_ERR)]),
         ins(fe, [i32(READ), i32(STATE)]))
    cond(eq(ec, i32(FIFO_READ_ERR)), eq(fe, i32(READ)))
    cond(ins(ec, [i32(FIFO_STATE_ERR), i32(FIFO_CNTR_ERR)]), eq(fe, i32(STATE)))
    # which_fifo_c
    cond(eq(ec, i32(SFIFO_OBSERVE_ERR)), eq(ff, i32(SFIFO_OBSERVE)))
    cond(eq(ec, i32(SFIFO_ESRNG_ERR)), eq(ff, i32(SFIFO_ESRNG)))
    cond(eq(ec, i32(SFIFO_DISTR_ERR)), eq(ff, i32(SFIFO_DISTR)))
    cond(eq(ec, i32(SFIFO_ESFINAL_ERR)), eq(ff, i32(SFIFO_ESFINAL)))
    cond(eq(ec, i32(FIFO_CNTR_ERR)),
         ins(ff, [i32(x) for x in (SFIFO_OBSERVE, SFIFO_ESRNG, SFIFO_DISTR, SFIFO_ESFINAL)]))
    # es_route_sw_c
    b.add_constraint(ins(var("es_route_sw"), [k(MUBI4_FALSE, 4), k(MUBI4_TRUE, 4)]))
    # fw_ov_rd_cnt_c
    b.add_constraint(dist_in(var("fw_ov_rd_cnt"), [i32(1), (i32(2), i32(127)), i32(128)]))
