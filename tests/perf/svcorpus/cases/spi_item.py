"""OpenTitan spi_agent: a SPI item's type-dependent dummy counts and lanes.

    rand spi_trans_type_e item_type;   // enum (int): Normal, SckNoCsb, CsbNoSck, IncompleteOpcode
    rand logic [7:0] data[$];
    rand int read_size;  rand logic [7:0] payload_q[$];  rand bit write_command;
    rand bit [7:0] address_q[$];  rand bit [7:0] opcode;  rand bit [2:0] num_lanes;
    rand int dummy_cycles, read_pipeline_mode;
    rand bit terminated_before_dummy_cycles, terminated_before_read_pipeline;
    rand uint dummy_sck_cnt, dummy_csb_length_ns;
    rand bit [CSB_WIDTH-1:0] csb_sel;  rand bit [3:0] plain_data_q[$];

    constraint data_size_c { data.size() inside {[0:65536]}; }
    constraint dummy_sck_cnt_c {
      if (item_type == SpiTransSckNoCsb) dummy_sck_cnt inside {[1:1000]};
      else dummy_sck_cnt == 0; }
    constraint dummy_csb_length_ns_c {
      if (item_type == SpiTransCsbNoSck) dummy_csb_length_ns inside {[1:1000]};
      else dummy_csb_length_ns == 0; }
    constraint num_lanes_c {
      write_command -> num_lanes == 1;
      num_lanes inside {0, 1, 2, 4}; }

CSB_WIDTH = 2. data.size() is the int variable `__Vdata_size` (Verilator's
name for it, so the capture lines up); the queue elements and the other
queues (payload_q, address_q, plain_data_q, all without a size constraint)
are not modelled. item_type is a rand enum: declared int-wide with the
membership {0..3} SV implies (Verilator emits it too). read_size, opcode,
dummy_cycles, read_pipeline_mode, the terminated_* bits and csb_sel are free
(not in the capture).
"""
from dv_solve.problem import BIN_EQ

from tests.perf.svcorpus.case import implies

NAME = "spi_item"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/dv/sv/spi_agent/spi_item.sv", "lines": "5-75"}
ABOUT = "enum-selected dummy counts (if/else ranges), lane count set with a write implication, queue size range"

VARS = [("item_type", 32, True), ("__Vdata_size", 32, True), ("read_size", 32, True),
        ("write_command", 1, False), ("opcode", 8, False), ("num_lanes", 3, False),
        ("dummy_cycles", 32, True), ("read_pipeline_mode", 32, True),
        ("terminated_before_dummy_cycles", 1, False), ("terminated_before_read_pipeline", 1, False),
        ("dummy_sck_cnt", 32, False), ("dummy_csb_length_ns", 32, False), ("csb_sel", 2, False)]
DIST = {}
SOLVE_BEFORE = []

SpiTransNormal, SpiTransSckNoCsb, SpiTransCsbNoSck, SpiTransIncompleteOpcode = range(4)


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    int_ = lambda k: b.expr_const(k, is_signed=True, width=32)   # noqa: E731
    # rand enum: only its named values
    b.add_constraint(b.expr_in_set(var("item_type"),
                                   [int_(k) for k in (SpiTransNormal, SpiTransSckNoCsb,
                                                      SpiTransCsbNoSck, SpiTransIncompleteOpcode)]))
    # data_size_c
    b.add_constraint(b.expr_in_range(var("__Vdata_size"), int_(0), int_(65536)))
    # dummy_sck_cnt_c, dummy_csb_length_ns_c
    for kind, n in ((SpiTransSckNoCsb, "dummy_sck_cnt"), (SpiTransCsbNoSck, "dummy_csb_length_ns")):
        b.add_constraint(b.expr_ite(b.expr_binary(BIN_EQ, var("item_type"), int_(kind)),
                                    b.expr_in_range(var(n), int_(1), int_(1000)),
                                    b.expr_binary(BIN_EQ, var(n), int_(0))))
    # num_lanes_c
    b.add_constraint(implies(b, var("write_command"),
                             b.expr_binary(BIN_EQ, var("num_lanes"), int_(1))))
    b.add_constraint(b.expr_in_set(var("num_lanes"), [int_(k) for k in (0, 1, 2, 4)]))
