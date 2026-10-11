"""OpenTitan uart: a uart_item and the uart_base_vseq DUT-configuration knobs.

uart_item (hw/dv/sv/uart_agent/uart_item.sv):

    rand bit start_bit = 1'b0;
    rand bit stop_bit = 1'b1;
    rand bit [7:0] data;
    rand bit parity;
    rand bit ovrd_en_parity = 1'b0;
    constraint start_bit_c { start_bit == 1'b0; }
    constraint stop_bit_c { stop_bit == 1'b1; }
    constraint ovrd_en_parity_c { ovrd_en_parity == 1'b0; }

uart_base_vseq (hw/ip/uart/dv/env/seq_lib/uart_base_vseq.sv):

    rand baud_rate_e baud_rate;     // enum int: 9600, 115200, 230400, 128000,
                                    //   256000, 1000000, 1500000
    rand bit en_tx, en_rx, en_parity, odd_parity, en_noise_filter;
    rand uint uart_period_glitch_pct;
    rand bit [NumUartIntr-1:0] en_intr;           // NumUartIntr = 9
    rand uint dly_to_access_fifo;
    constraint uart_period_glitch_pct_c { uart_period_glitch_pct inside {[0:10]}; }
    constraint baud_rate_c {
      if (baud_rate == BaudRate1p5Mbps && p_sequencer.cfg.clk_freq_mhz < 48) {
        en_noise_filter == 0;
        uart_period_glitch_pct == 0;
      }
      `CALC_NCO(baud_rate, p_sequencer.cfg.ral.ctrl.nco.get_n_bits(),
          p_sequencer.cfg.clk_freq_mhz) < 2 ** p_sequencer.cfg.ral.ctrl.nco.get_n_bits();
    }
    constraint dly_to_access_fifo_c {
      dly_to_access_fifo dist { 0 :/ 1, [1:100] :/ 1, [101:10_000] :/ 8,
                                [10_001:1_000_000] :/ 1 };
    }

    `define CALC_NCO(baud_rate, nco_width, clk_freq_mhz) \
      (baud_rate == BaudRate1p5Mbps && clk_freq_mhz == 24) ? 16'hffff : \
          (longint'(baud_rate) * (2**(nco_width+4))) / (clk_freq_mhz * 1000_000)

The two classes are merged into one in the harness so one randomize() covers
both. Non-rand state: cfg.clk_freq_mhz (itself rand in dv_base_env_cfg, a
`dist` over [5:100]) is fixed at 24 MHz, a value in its distribution that
reaches both 1.5 Mbps special cases; nco.get_n_bits() is 16 (CTRL.NCO is
bits 31:16), held in a `uint` member. The macro expands with `?:` loosest, so
the constraint reads `cond ? 16'hffff : (nco < 2**16)` (as Verilator also
parses it): the 1.5 Mbps-at-24 MHz case is unconditionally allowed. The
builder has no `**`: 2**(16+4) and 2**16 are written as their int values.
"""
from dv_solve.problem import BIN_AND, BIN_DIV, BIN_EQ, BIN_LT, BIN_MUL, BIN_NEQ

from tests.perf.svcorpus.case import all_of, implies, inside_dist

NAME = "uart_base"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/dv/sv/uart_agent/uart_item.sv + hw/ip/uart/dv/env/seq_lib/uart_base_vseq.sv",
          "lines": "uart_item 8-34; uart_base_vseq 11-55; uart_env_pkg 89-91"}
ABOUT = "uart item framing pins plus baud-rate enum, an NCO-overflow quotient and a 4-band delay dist"

VARS = [("start_bit", 1, False), ("stop_bit", 1, False), ("data", 8, False),
        ("parity", 1, False), ("ovrd_en_parity", 1, False),
        ("baud_rate", 32, True), ("en_tx", 1, False), ("en_rx", 1, False),
        ("en_parity", 1, False), ("odd_parity", 1, False), ("en_noise_filter", 1, False),
        ("uart_period_glitch_pct", 32, False), ("en_intr", 9, False),
        ("dly_to_access_fifo", 32, False)]
DIST = {"dly_to_access_fifo": [(0, 0, 1, False), (1, 100, 1, False), (101, 10_000, 8, False),
                               (10_001, 1_000_000, 1, False)]}
SOLVE_BEFORE = []
KNOWN_ISSUE = "B105: 64-bit unsigned division is CompileIncomplete on the builder path"

BAUD_RATES = [9600, 115200, 230400, 128000, 256000, 1000000, 1500000]
BaudRate1p5Mbps = 1500000
CLK_FREQ_MHZ = 24


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    int_ = lambda k: b.expr_const(k, is_signed=True, width=32)   # noqa: E731
    uint = lambda k: b.expr_const(k, width=32)              # noqa: E731
    bit = lambda k: b.expr_const(k, width=1)                # noqa: E731
    eq = lambda x, y: b.expr_binary(BIN_EQ, x, y)          # noqa: E731
    br = var("baud_rate")
    # rand baud_rate_e: one of the enum's values
    b.add_constraint(b.expr_in_set(br, [int_(k) for k in BAUD_RATES]))
    b.add_constraint(eq(var("start_bit"), bit(0)))
    b.add_constraint(eq(var("stop_bit"), bit(1)))
    b.add_constraint(eq(var("ovrd_en_parity"), bit(0)))
    b.add_constraint(b.expr_in_range(var("uart_period_glitch_pct"), int_(0), int_(10)))
    clk = uint(CLK_FREQ_MHZ)
    b.add_constraint(implies(b, b.expr_binary(BIN_AND, eq(br, int_(BaudRate1p5Mbps)),
                                              b.expr_binary(BIN_LT, clk, int_(48))),
                             all_of(b, eq(var("en_noise_filter"), int_(0)),
                                    eq(var("uart_period_glitch_pct"), int_(0)))))
    nco = b.expr_binary(BIN_DIV,
                        b.expr_binary(BIN_MUL, b.expr_cast(br, 64, True), int_(1 << 20)),
                        b.expr_binary(BIN_MUL, uint(CLK_FREQ_MHZ), int_(1000_000)))
    calc = b.expr_ite(b.expr_binary(BIN_AND, eq(br, int_(BaudRate1p5Mbps)), eq(clk, int_(24))),
                      b.expr_const(0xFFFF, width=16),
                      b.expr_binary(BIN_LT, nco, int_(1 << 16)))
    b.add_constraint(b.expr_binary(BIN_NEQ, calc, b.expr_const(0, width=16)))
    b.add_constraint(inside_dist(b, var("dly_to_access_fifo"), DIST["dly_to_access_fifo"]))
