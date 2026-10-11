"""OpenTitan push_pull_agent: a push/pull handshake item's data and delays.

    class push_pull_item #(parameter int HostDataWidth = 32,
                           parameter int DeviceDataWidth = HostDataWidth);
      rand bit [HostDataWidth-1:0]   h_data;
      rand bit [DeviceDataWidth-1:0] d_data;
      rand int unsigned host_delay;
      constraint host_delay_max_c   { soft host_delay <= 1000; }
      rand int unsigned device_delay;
      constraint device_delay_max_c { soft device_delay <= 1000; }
      rand int unsigned req_lo_delay;
      constraint req_lo_delay_max_c { soft req_lo_delay <= 1000; }
      rand int unsigned ack_lo_delay;
      constraint ack_lo_delay_max_c { soft ack_lo_delay <= 1000; }

Parameters: the cip_base EDN agent's instantiation,
push_pull_item #(.DeviceDataWidth(EDN_DATA_WIDTH)) with EDN_DATA_WIDTH = 33
(32 data bits + 1 fips bit), HostDataWidth left at its default 32.
h_data / d_data are unconstrained (not in the capture). All four constraints
are soft; Verilator's first query asserts them (it relaxes softs only on unsat).
"""
from dv_solve.problem import BIN_LTE

NAME = "push_pull_item"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/dv/sv/push_pull_agent/push_pull_item.sv", "lines": "5-34"}
ABOUT = "four soft upper bounds on handshake delays; two free data words (32 and 33 bits)"

VARS = [("h_data", 32, False), ("d_data", 33, False), ("host_delay", 32, False),
        ("device_delay", 32, False), ("req_lo_delay", 32, False), ("ack_lo_delay", 32, False)]
DIST = {}
SOLVE_BEFORE = []


def build(b, v):
    names = ("host_delay", "device_delay", "req_lo_delay", "ack_lo_delay")
    # SV ranks a later soft constraint above an earlier one (builder: 0 = kept longest)
    for i, n in enumerate(names):
        b.add_soft_constraint(b.expr_binary(BIN_LTE, b.expr_var(v[n]),
                                            b.expr_const(1000, is_signed=True, width=32)),
                              priority=len(names) - 1 - i)
