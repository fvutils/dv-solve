"""OpenTitan spi_host: the base vseq's register config (configopts, ctrl) and loop/delay knobs.

    rand spi_host_command_t spi_host_command_reg;  rand spi_host_ctrl_t spi_host_ctrl_reg;
    rand spi_host_configopts_t spi_config_regs;    rand spi_host_event_enable_t event_enable;
    rand spi_host_intr_enable_t intr_enable;       rand spi_host_error_enable_t error_enable;
    rand uint num_runs, tx_fifo_access_dly, rx_fifo_access_dly, clear_intr_dly;
    constraint num_trans_c {
      num_trans inside {[cfg.seq_cfg.host_spi_min_trans : cfg.seq_cfg.host_spi_max_trans]}; }
    constraint num_runs_c {
      num_runs inside {[cfg.seq_cfg.host_spi_min_runs : cfg.seq_cfg.host_spi_max_runs]}; }
    constraint intr_dly_c {
      clear_intr_dly inside {[cfg.seq_cfg.host_spi_min_dly : cfg.seq_cfg.host_spi_max_dly]}; }
    constraint fifo_dly_c {
      rx_fifo_access_dly inside {[cfg.seq_cfg.host_spi_min_dly : cfg.seq_cfg.host_spi_max_dly]};
      tx_fifo_access_dly inside {[cfg.seq_cfg.host_spi_min_dly : cfg.seq_cfg.host_spi_max_dly]}; }
    constraint spi_config_regs_c {
      spi_config_regs.cpol dist { 1'b0 :/ 1, 1'b1 :/ 1 };
      spi_config_regs.cpha dist { 1'b0 :/ 1, 1'b1 :/ 1 };
      spi_config_regs.csnlead inside {[cfg.seq_cfg.host_spi_min_csn_latency :
                                       cfg.seq_cfg.host_spi_max_csn_latency]};
      ... csntrail, csnidle the same ... }
    constraint spi_config_regs_clkdiv_c {
      spi_config_regs.clkdiv dist {
        [cfg.seq_cfg.host_spi_min_clkdiv : cfg.seq_cfg.host_spi_lower_middle_clkdiv]      :/90,
        [cfg.seq_cfg.host_spi_lower_middle_clkdiv+1 : cfg.seq_cfg.host_spi_middle_clkdiv] :/7,
        [cfg.seq_cfg.host_spi_middle_clkdiv+1 : cfg.seq_cfg.host_spi_upper_middle_clkdiv] :/3 }; }
    constraint spi_ctrl_regs_c {
      spi_host_ctrl_reg.csid inside {[0 : SPI_HOST_NUM_CS-1]};
      spi_host_ctrl_reg.tx_watermark dist { [1:7] :/ 1, [8:15] :/ 1, [16:31] :/ 1,
                                            [32:cfg.seq_cfg.host_spi_max_txwm] :/ 1 };
      spi_host_ctrl_reg.rx_watermark dist { [1:7] :/ 1, [8:15] :/ 1, [16:31] :/ 1,
                                            [32:cfg.seq_cfg.host_spi_max_rxwm] :/ 1 }; }

The struct types (spi_host_env_pkg): configopts {cpol, cpha, fullcyc, csnlead[3:0],
csntrail[3:0], csnidle[3:0], clkdiv[15:0]}; ctrl {spien, output_en, sw_rst,
csid[31:0], tx_watermark[8:0], rx_watermark[6:0]}; command {spi_mode_e mode,
spi_dir_e direction, csaat, len[19:0]} (both enums bit [1:0], all four
values named); event_enable/intr_enable/error_enable are packed structs of
6/2/5 bits with no rand members of their own, randomized whole. Struct
members are named <field>.<member>, as in the capture. num_trans is the
dv_base_vseq rand field; this class overrides its num_trans_c.
SPI_HOST_NUM_CS = 1 (integer unsigned), SPI_HOST_TX_DEPTH = 72,
SPI_HOST_RX_DEPTH = 64.

Non-rand state as constants: cfg.seq_cfg.* are spi_host_seq_cfg's defaults
(uint): trans [2:5], runs [2:3], dly [0:5], csn latency [0:15], clkdiv
min/lower-middle/middle/upper-middle 0/'hF/'hF0/'hFF, max_txwm 72, max_rxwm
64. Constant bound expressions (SPI_HOST_NUM_CS-1, ..._clkdiv+1) are folded
to literals of their SV type (uint). Verilator leaves the unconstrained
fields (command reg, the packed enables) out of the query.
"""
NAME = "spi_host_base"
SOURCE = {"repo": "lowRISC/opentitan", "commit": "c9ef9e57e7ed2e99b2a315289a142bd2e6b6db36",
          "file": "hw/ip/spi_host/dv/env/seq_lib/spi_host_base_vseq.sv", "lines": "15-95"}
ABOUT = "SPI host CSR config: loop/delay windows, chip-select latencies, banded clkdiv and watermark dists"

SPI_HOST_NUM_CS = 1
MIN_TRANS, MAX_TRANS, MIN_RUNS, MAX_RUNS = 2, 5, 2, 3
MIN_CSN, MAX_CSN, MIN_DLY, MAX_DLY = 0, 15, 0, 5
MIN_CLKDIV, LOWER_MIDDLE_CLKDIV, MIDDLE_CLKDIV, UPPER_MIDDLE_CLKDIV = 0, 0xF, 0xF0, 0xFF
MAX_TXWM, MAX_RXWM = 72, 64

VARS = [("num_trans", 32, False),
        ("spi_host_command_reg.mode", 2, False), ("spi_host_command_reg.direction", 2, False),
        ("spi_host_command_reg.csaat", 1, False), ("spi_host_command_reg.len", 20, False),
        ("spi_host_ctrl_reg.spien", 1, False), ("spi_host_ctrl_reg.output_en", 1, False),
        ("spi_host_ctrl_reg.sw_rst", 1, False), ("spi_host_ctrl_reg.csid", 32, False),
        ("spi_host_ctrl_reg.tx_watermark", 9, False), ("spi_host_ctrl_reg.rx_watermark", 7, False),
        ("spi_config_regs.cpol", 1, False), ("spi_config_regs.cpha", 1, False),
        ("spi_config_regs.fullcyc", 1, False), ("spi_config_regs.csnlead", 4, False),
        ("spi_config_regs.csntrail", 4, False), ("spi_config_regs.csnidle", 4, False),
        ("spi_config_regs.clkdiv", 16, False),
        ("event_enable", 6, False), ("intr_enable", 2, False), ("error_enable", 5, False),
        ("num_runs", 32, False), ("tx_fifo_access_dly", 32, False),
        ("rx_fifo_access_dly", 32, False), ("clear_intr_dly", 32, False)]


def _wm(mx):
    return [(1, 7, 1, False), (8, 15, 1, False), (16, 31, 1, False), (32, mx, 1, False)]


DIST = {"spi_config_regs.cpol": [(0, 0, 1, False), (1, 1, 1, False)],
        "spi_config_regs.cpha": [(0, 0, 1, False), (1, 1, 1, False)],
        "spi_config_regs.clkdiv": [(MIN_CLKDIV, LOWER_MIDDLE_CLKDIV, 90, False),
                                   (LOWER_MIDDLE_CLKDIV + 1, MIDDLE_CLKDIV, 7, False),
                                   (MIDDLE_CLKDIV + 1, UPPER_MIDDLE_CLKDIV, 3, False)],
        "spi_host_ctrl_reg.tx_watermark": _wm(MAX_TXWM),
        "spi_host_ctrl_reg.rx_watermark": _wm(MAX_RXWM)}
SOLVE_BEFORE = []


def build(b, v):
    var = lambda n: b.expr_var(v[n])                       # noqa: E731
    i32 = lambda x: b.expr_const(x, is_signed=True, width=32)   # unsized literal
    u32 = lambda x: b.expr_const(x, width=32)              # uint knob / parameter
    rng = lambda n, lo, hi: b.add_constraint(b.expr_in_range(var(n), lo, hi))   # noqa: E731

    # rand enums take only their named values (all four 2-bit codes are named)
    b.add_constraint(b.expr_in_set(var("spi_host_command_reg.mode"),
                                   [b.expr_const(x, width=2) for x in range(4)]))
    b.add_constraint(b.expr_in_set(var("spi_host_command_reg.direction"),
                                   [b.expr_const(x, width=2) for x in range(4)]))

    rng("num_trans", u32(MIN_TRANS), u32(MAX_TRANS))                  # num_trans_c
    rng("num_runs", u32(MIN_RUNS), u32(MAX_RUNS))                     # num_runs_c
    rng("clear_intr_dly", u32(MIN_DLY), u32(MAX_DLY))                 # intr_dly_c
    rng("rx_fifo_access_dly", u32(MIN_DLY), u32(MAX_DLY))             # fifo_dly_c
    rng("tx_fifo_access_dly", u32(MIN_DLY), u32(MAX_DLY))
    # spi_config_regs_c
    for n in ("spi_config_regs.cpol", "spi_config_regs.cpha"):
        b.add_constraint(b.expr_in_set(var(n), [b.expr_const(0, width=1),
                                                b.expr_const(1, width=1)]))
    for n in ("spi_config_regs.csnlead", "spi_config_regs.csntrail", "spi_config_regs.csnidle"):
        rng(n, u32(MIN_CSN), u32(MAX_CSN))
    # spi_config_regs_clkdiv_c
    b.add_constraint(b.expr_in_ranges(var("spi_config_regs.clkdiv"), [
        (u32(MIN_CLKDIV), u32(LOWER_MIDDLE_CLKDIV)),
        (u32(LOWER_MIDDLE_CLKDIV + 1), u32(MIDDLE_CLKDIV)),
        (u32(MIDDLE_CLKDIV + 1), u32(UPPER_MIDDLE_CLKDIV))]))
    # spi_ctrl_regs_c
    rng("spi_host_ctrl_reg.csid", i32(0), u32(SPI_HOST_NUM_CS - 1))
    for n, mx in (("spi_host_ctrl_reg.tx_watermark", MAX_TXWM),
                  ("spi_host_ctrl_reg.rx_watermark", MAX_RXWM)):
        b.add_constraint(b.expr_in_ranges(var(n), [(i32(1), i32(7)), (i32(8), i32(15)),
                                                   (i32(16), i32(31)), (i32(32), u32(mx))]))
