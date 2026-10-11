"""OpenTitan alert_esc_agent: an alert/escalation item's mode bits and delays.

    rand bit s_alert_send, s_alert_ping_rsp, r_alert_rsp, r_alert_ping_send,
             r_esc_rsp, int_err, standalone_int_err, ping_timeout;
    rand alert_sig_int_err_e alert_int_err_type;    // enum bit [1:0], 4 values
    rand alert_esc_trans_type_e alert_esc_type;     // enum (int), 3 values
    rand alert_handshake_e alert_handshake_sta;     // enum (int), 5 values
    rand esc_handshake_e esc_handshake_sta;         // enum (int), 10 values
    rand int sig_cycle_cnt;
    rand int unsigned ping_delay, ack_delay, ack_stable, alert_delay, int_err_cyc;

    constraint delay_c {
      soft ping_delay  dist {0 :/ 5, [1:10] :/ 5};
      soft ack_delay   dist {0 :/ 5, [1:10] :/ 5};
      soft alert_delay dist {0 :/ 5, [1:10] :/ 5};
      soft ack_stable  dist {1 :/ 5, [2:10] :/ 5};
      soft int_err_cyc dist {1 :/ 5, [2:10] :/ 5}; }
    constraint alert_esc_mode_c {
      r_esc_rsp == 1 -> (!s_alert_send && !r_alert_rsp && !r_alert_ping_send && !s_alert_ping_rsp);
      (s_alert_send || r_alert_rsp || r_alert_ping_send || s_alert_ping_rsp) -> !r_esc_rsp; }
    constraint sig_int_err_c { alert_int_err_type == NoAlertBeforeAfterIntFail; }

The `soft ... dist` memberships are soft constraints here (SV meaning), with
the weights in DIST; Verilator 5.052 asserts them as hard in its query.
alert_esc_type, alert_handshake_sta and esc_handshake_sta appear in no
constraint, and Verilator leaves them out of the solver query (it draws them
in range by itself), so they are not modelled: their enum membership would be
a constraint the capture does not have. alert_int_err_type does appear, so it
has its (full-domain) membership, as Verilator emits. int_err,
standalone_int_err, ping_timeout and sig_cycle_cnt are free.
"""
from dv_solve.problem import BIN_AND, BIN_EQ, BIN_OR, UN_NOT

from tests.perf.svcorpus.case import implies, inside_dist

NAME = "alert_esc_seq_item"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/dv/sv/alert_esc_agent/alert_esc_seq_item.sv", "lines": "9-36, 68-85"}
ABOUT = "alert vs escalation mode exclusion over five bits; five soft-dist delays; a pinned enum"

VARS = [("s_alert_send", 1, False), ("s_alert_ping_rsp", 1, False), ("r_alert_rsp", 1, False),
        ("r_alert_ping_send", 1, False), ("r_esc_rsp", 1, False), ("int_err", 1, False),
        ("standalone_int_err", 1, False), ("ping_timeout", 1, False),
        ("alert_int_err_type", 2, False), ("sig_cycle_cnt", 32, True),
        ("ping_delay", 32, False), ("ack_delay", 32, False), ("ack_stable", 32, False),
        ("alert_delay", 32, False), ("int_err_cyc", 32, False)]
_D0 = [(0, 0, 5, False), (1, 10, 5, False)]
_D1 = [(1, 1, 5, False), (2, 10, 5, False)]
DIST = {"ping_delay": _D0, "ack_delay": _D0, "alert_delay": _D0,
        "ack_stable": _D1, "int_err_cyc": _D1}
SOLVE_BEFORE = []

NoAlertBeforeAfterIntFail = 0


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    int_ = lambda k: b.expr_const(k, is_signed=True, width=32)   # noqa: E731
    not_ = lambda x: b.expr_unary(UN_NOT, x)               # noqa: E731
    # rand enum: only its named values
    b.add_constraint(b.expr_in_set(var("alert_int_err_type"),
                                   [b.expr_const(k, width=2) for k in range(4)]))
    # delay_c: SV ranks a later soft constraint above an earlier one (builder: 0 = kept longest)
    names = ("ping_delay", "ack_delay", "alert_delay", "ack_stable", "int_err_cyc")
    for i, n in enumerate(names):
        b.add_soft_constraint(inside_dist(b, var(n), DIST[n]), priority=len(names) - 1 - i)
    # alert_esc_mode_c
    sa, sp, ra, rp, esc = (var(n) for n in ("s_alert_send", "s_alert_ping_rsp", "r_alert_rsp",
                                            "r_alert_ping_send", "r_esc_rsp"))
    none = b.expr_binary(BIN_AND, b.expr_binary(BIN_AND, b.expr_binary(BIN_AND, not_(sa), not_(ra)),
                                                not_(rp)), not_(sp))
    b.add_constraint(implies(b, b.expr_binary(BIN_EQ, esc, int_(1)), none))
    anyof = b.expr_binary(BIN_OR, b.expr_binary(BIN_OR, b.expr_binary(BIN_OR, sa, ra), rp), sp)
    b.add_constraint(implies(b, anyof, not_(esc)))
    # sig_int_err_c
    b.add_constraint(b.expr_binary(BIN_EQ, var("alert_int_err_type"),
                                   b.expr_const(NoAlertBeforeAfterIntFail, width=2)))
