// OpenTitan hw/ip/spi_host/dv/env/seq_lib/spi_host_base_vseq.sv: rand fields + constraint blocks
// (UVM stripped; cfg.seq_cfg.* knobs are spi_host_seq_cfg's defaults, as constants of a seq_cfg
// object; num_trans is the dv_base_vseq rand field whose num_trans_c this class overrides)
package p;
  typedef int unsigned uint;
  parameter int TxDepth = 72;
  parameter int RxDepth = 64;
  parameter integer unsigned SPI_HOST_NUM_CS = 1;
  parameter uint SPI_HOST_TX_DEPTH = TxDepth;
  parameter uint SPI_HOST_RX_DEPTH = RxDepth;
  parameter uint SPI_HOST_COMMAND_LEN_SIZE_BITS = 20;
  typedef enum bit [1:0] { Standard = 2'b00, Dual = 2'b01, Quad = 2'b10, RsvdSpd = 2'b11 } spi_mode_e;
  typedef enum bit [1:0] { None = 2'b00, RxOnly = 2'b01, TxOnly = 2'b10, Bidir = 2'b11 } spi_dir_e;
  typedef struct {
    rand bit        cpol;
    rand bit        cpha;
    rand bit        fullcyc;
    rand bit [3:0]  csnlead;
    rand bit [3:0]  csntrail;
    rand bit [3:0]  csnidle;
    rand bit [15:0] clkdiv;
  } spi_host_configopts_t;
  typedef struct {
    rand bit spien;
    rand bit output_en;
    rand bit sw_rst;
    rand bit [31:0] csid;
    rand bit [8:0]  tx_watermark;
    rand bit [6:0]  rx_watermark;
  } spi_host_ctrl_t;
  typedef struct {
    rand spi_mode_e mode;
    rand spi_dir_e  direction;
    rand bit        csaat;
    rand bit [SPI_HOST_COMMAND_LEN_SIZE_BITS-1:0] len;
  } spi_host_command_t;
  typedef struct packed { bit csidinval; bit cmdinval; bit underflow; bit overflow; bit cmdbusy; } spi_host_error_enable_t;
  typedef struct packed { bit idle; bit ready; bit txwm; bit rxwm; bit txempty; bit rxfull; } spi_host_event_enable_t;
  typedef struct packed { bit spi_event; bit error; } spi_host_intr_enable_t;
endpackage
import p::*;
class spi_host_seq_cfg;
  uint host_spi_min_trans             = 2;
  uint host_spi_max_trans             = 5;
  uint host_spi_min_runs              = 2;
  uint host_spi_max_runs              = 3;
  uint host_spi_min_csn_latency       = 0;
  uint host_spi_max_csn_latency       = 15;
  uint host_spi_min_clkdiv            = 0;
  uint host_spi_lower_middle_clkdiv   = 16'hF;
  uint host_spi_middle_clkdiv         = 16'hF0;
  uint host_spi_upper_middle_clkdiv   = 16'hFF;
  uint host_spi_max_clkdiv            = 16'hFFFF;
  uint host_spi_min_dly               = 0;
  uint host_spi_max_dly               = 5;
  uint host_spi_max_rxwm              = SPI_HOST_RX_DEPTH;
  uint host_spi_max_txwm              = SPI_HOST_TX_DEPTH;
endclass
class spi_host_env_cfg;
  spi_host_seq_cfg seq_cfg = new;
endclass
class spi_host_base_vseq;
  spi_host_env_cfg cfg = new;
  rand uint num_trans;
  rand spi_host_command_t    spi_host_command_reg;
  rand spi_host_ctrl_t       spi_host_ctrl_reg;
  rand spi_host_configopts_t spi_config_regs;
  rand spi_host_event_enable_t event_enable;
  rand spi_host_intr_enable_t intr_enable;
  rand spi_host_error_enable_t error_enable;
  rand uint                  num_runs;
  rand uint                  tx_fifo_access_dly;
  rand uint                  rx_fifo_access_dly;
  rand uint                  clear_intr_dly;

  constraint num_trans_c {
    num_trans inside {[cfg.seq_cfg.host_spi_min_trans : cfg.seq_cfg.host_spi_max_trans]};
  }
  constraint num_runs_c {
    num_runs inside {[cfg.seq_cfg.host_spi_min_runs : cfg.seq_cfg.host_spi_max_runs]};
  }
  constraint intr_dly_c {
    clear_intr_dly inside {[cfg.seq_cfg.host_spi_min_dly : cfg.seq_cfg.host_spi_max_dly]};
  }
  constraint fifo_dly_c {
    rx_fifo_access_dly inside {[cfg.seq_cfg.host_spi_min_dly : cfg.seq_cfg.host_spi_max_dly]};
    tx_fifo_access_dly inside {[cfg.seq_cfg.host_spi_min_dly : cfg.seq_cfg.host_spi_max_dly]};
  }
  constraint spi_config_regs_c {
      spi_config_regs.cpol dist {
        1'b0 :/ 1,
        1'b1 :/ 1
      };
      spi_config_regs.cpha dist {
        1'b0 :/ 1,
        1'b1 :/ 1
      };
      spi_config_regs.csnlead inside {[cfg.seq_cfg.host_spi_min_csn_latency :
                                       cfg.seq_cfg.host_spi_max_csn_latency]};
      spi_config_regs.csntrail inside {[cfg.seq_cfg.host_spi_min_csn_latency :
                                        cfg.seq_cfg.host_spi_max_csn_latency]};
      spi_config_regs.csnidle inside {[cfg.seq_cfg.host_spi_min_csn_latency :
                                       cfg.seq_cfg.host_spi_max_csn_latency]};
  }
  constraint spi_config_regs_clkdiv_c {
    spi_config_regs.clkdiv dist {
      [cfg.seq_cfg.host_spi_min_clkdiv : cfg.seq_cfg.host_spi_lower_middle_clkdiv]      :/90,
      [cfg.seq_cfg.host_spi_lower_middle_clkdiv+1 : cfg.seq_cfg.host_spi_middle_clkdiv] :/7,
      [cfg.seq_cfg.host_spi_middle_clkdiv+1 : cfg.seq_cfg.host_spi_upper_middle_clkdiv] :/3
    };
  }
  constraint spi_ctrl_regs_c {
    spi_host_ctrl_reg.csid inside {[0 : SPI_HOST_NUM_CS-1]};
    spi_host_ctrl_reg.tx_watermark dist {
      [1:7]   :/ 1,
      [8:15]  :/ 1,
      [16:31] :/ 1,
      [32:cfg.seq_cfg.host_spi_max_txwm] :/ 1
      };
    spi_host_ctrl_reg.rx_watermark dist {
      [1:7]   :/ 1,
      [8:15]  :/ 1,
      [16:31] :/ 1,
      [32:cfg.seq_cfg.host_spi_max_rxwm] :/ 1
      };
  }
endclass
module top;
  initial begin
    spi_host_base_vseq s = new;
    for (int i = 0; i < 20; i++) begin
      if (!s.randomize()) $display("FAIL");
      $display("%0d nt=%0d nr=%0d dly=%0d/%0d/%0d cfg=%b%b%b %0d %0d %0d clkdiv=%0d ctrl=%b%b%b csid=%0d wm=%0d/%0d cmd=%0d %0d %b %0d ev=%b ie=%b ee=%b",
        i, s.num_trans, s.num_runs, s.tx_fifo_access_dly, s.rx_fifo_access_dly, s.clear_intr_dly,
        s.spi_config_regs.cpol, s.spi_config_regs.cpha, s.spi_config_regs.fullcyc,
        s.spi_config_regs.csnlead, s.spi_config_regs.csntrail, s.spi_config_regs.csnidle,
        s.spi_config_regs.clkdiv, s.spi_host_ctrl_reg.spien, s.spi_host_ctrl_reg.output_en,
        s.spi_host_ctrl_reg.sw_rst, s.spi_host_ctrl_reg.csid, s.spi_host_ctrl_reg.tx_watermark,
        s.spi_host_ctrl_reg.rx_watermark, s.spi_host_command_reg.mode,
        s.spi_host_command_reg.direction, s.spi_host_command_reg.csaat, s.spi_host_command_reg.len,
        s.event_enable, s.intr_enable, s.error_enable);
    end
    $finish;
  end
endmodule
