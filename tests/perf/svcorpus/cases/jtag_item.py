"""OpenTitan jtag_agent: a JTAG IR/DR item's lengths, pause points and knobs.

    rand uint ir_len, dr_len;  rand logic [JTAG_IRW-1:0] ir;
    rand logic [JTAG_DRW-1:0] dr, dout;  rand bus_op_e bus_op;
    rand bit skip_reselected_ir;
    rand uint ir_pause_count, dr_pause_count, dr_pause_cycle, ir_pause_cycle;
    rand bit dummy_ir, dummy_dr, exit_via_pause_ir, exit_via_pause_dr,
             exit_to_rti_dr, reset_tap_fsm;
    constraint ir_len_c          { ir_len <= JTAG_IRW; }
    constraint dr_len_c          { dr_len <= JTAG_DRW; }
    constraint exit_to_rti_dr_c  { soft exit_to_rti_dr == 1; }
    constraint reset_tap_fsm_c   { soft reset_tap_fsm == 0; }
    constraint ir_dr_non_zero_c  { ir_len > 0 || dr_len > 0; }
    constraint skip_reselected_ir_c { skip_reselected_ir dist {0:/3, 1:/7}; }
    constraint ir_pause_count_c  { ir_pause_count dist {0:/80, [1:40]:/ 20}; }
    constraint dr_pause_count_c  { dr_pause_count dist {0:/ 80, [1:40]:/ 20}; }
    constraint dr_pause_cycle_c {
      if (dr_pause_count > 0 && dr_len > 3) dr_pause_cycle inside {[1:dr_len-3]};
      else dr_pause_cycle == 0;
      solve dr_len before dr_pause_cycle;
      solve dr_pause_count before dr_pause_cycle; }
    constraint ir_pause_cycle_c { (the same over ir_*) }
    constraint dummy_ir_c          { dummy_ir dist { 0 := 99, 1 := 1}; }
    constraint dummy_dr_c          { soft dummy_dr == 0; }
    constraint exit_via_pause_ir_c { exit_via_pause_ir dist { 0 := 90, 1 := 10}; }
    constraint exit_via_pause_dr_c { exit_via_pause_dr dist { 0 := 90, 1 := 10}; }

JTAG_IRW = 32, JTAG_DRW = 64 (`parameter uint`, i.e. bit [31:0]). bus_op is a
1-bit enum whose two values cover its domain, so it has no membership
constraint (Verilator emits none). ir, dr, dout, bus_op are free (not in the
capture).
"""
from dv_solve.problem import BIN_AND, BIN_EQ, BIN_GT, BIN_LTE, BIN_OR, BIN_SUB

from tests.perf.svcorpus.case import inside_dist

NAME = "jtag_item"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/dv/sv/jtag_agent/jtag_item.sv", "lines": "5-125"}
ABOUT = "IR/DR lengths with dist'd pause counts; pause cycle ranged by length when pausing; dist/soft knob bits"

VARS = [("ir_len", 32, False), ("dr_len", 32, False), ("ir", 32, False), ("dr", 64, False),
        ("dout", 64, False), ("bus_op", 1, False), ("skip_reselected_ir", 1, False),
        ("ir_pause_count", 32, False), ("dr_pause_count", 32, False),
        ("dr_pause_cycle", 32, False), ("ir_pause_cycle", 32, False),
        ("dummy_ir", 1, False), ("dummy_dr", 1, False), ("exit_via_pause_ir", 1, False),
        ("exit_via_pause_dr", 1, False), ("exit_to_rti_dr", 1, False),
        ("reset_tap_fsm", 1, False)]
DIST = {"skip_reselected_ir": [(0, 0, 3, False), (1, 1, 7, False)],
        "ir_pause_count": [(0, 0, 80, False), (1, 40, 20, False)],
        "dr_pause_count": [(0, 0, 80, False), (1, 40, 20, False)],
        "dummy_ir": [(0, 0, 99, True), (1, 1, 1, True)],
        "exit_via_pause_ir": [(0, 0, 90, True), (1, 1, 10, True)],
        "exit_via_pause_dr": [(0, 0, 90, True), (1, 1, 10, True)]}
SOLVE_BEFORE = [(("dr_len",), ("dr_pause_cycle",)), (("dr_pause_count",), ("dr_pause_cycle",)),
                (("ir_len",), ("ir_pause_cycle",)), (("ir_pause_count",), ("ir_pause_cycle",))]

JTAG_IRW, JTAG_DRW = 32, 64


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    int_ = lambda k: b.expr_const(k, is_signed=True, width=32)   # noqa: E731
    uint = lambda k: b.expr_const(k, width=32)             # noqa: E731
    softs = []
    b.add_constraint(b.expr_binary(BIN_LTE, var("ir_len"), uint(JTAG_IRW)))
    b.add_constraint(b.expr_binary(BIN_LTE, var("dr_len"), uint(JTAG_DRW)))
    softs.append(b.expr_binary(BIN_EQ, var("exit_to_rti_dr"), int_(1)))
    softs.append(b.expr_binary(BIN_EQ, var("reset_tap_fsm"), int_(0)))
    b.add_constraint(b.expr_binary(BIN_OR, b.expr_binary(BIN_GT, var("ir_len"), int_(0)),
                                   b.expr_binary(BIN_GT, var("dr_len"), int_(0))))
    for n in ("skip_reselected_ir", "ir_pause_count", "dr_pause_count"):
        b.add_constraint(inside_dist(b, var(n), DIST[n]))
    for x in ("dr", "ir"):                                  # dr_pause_cycle_c, ir_pause_cycle_c
        cnt, ln, cyc = var(f"{x}_pause_count"), var(f"{x}_len"), var(f"{x}_pause_cycle")
        cond = b.expr_binary(BIN_AND, b.expr_binary(BIN_GT, cnt, int_(0)),
                             b.expr_binary(BIN_GT, ln, int_(3)))
        b.add_constraint(b.expr_ite(cond,
                                    b.expr_in_range(cyc, int_(1), b.expr_binary(BIN_SUB, ln, int_(3))),
                                    b.expr_binary(BIN_EQ, cyc, int_(0))))
    b.add_constraint(inside_dist(b, var("dummy_ir"), DIST["dummy_ir"]))
    softs.append(b.expr_binary(BIN_EQ, var("dummy_dr"), int_(0)))
    for n in ("exit_via_pause_ir", "exit_via_pause_dr"):
        b.add_constraint(inside_dist(b, var(n), DIST[n]))
    # SV: a later soft constraint outranks an earlier one (builder: 0 = kept longest)
    for i, s in enumerate(softs):
        b.add_soft_constraint(s, priority=len(softs) - 1 - i)
