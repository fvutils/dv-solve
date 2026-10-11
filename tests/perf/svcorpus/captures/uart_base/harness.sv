// OpenTitan hw/dv/sv/uart_agent/uart_item.sv (start_bit_c, stop_bit_c, ovrd_en_parity_c) and
// hw/ip/uart/dv/env/seq_lib/uart_base_vseq.sv (uart_period_glitch_pct_c, baud_rate_c,
// dly_to_access_fifo_c), merged into one class so one randomize() covers both.
package p;
  typedef int unsigned uint;
  typedef enum int {
    BaudRate9600    = 9600,
    BaudRate115200  = 115200,
    BaudRate230400  = 230400,
    BaudRate128Kbps = 128000,
    BaudRate256Kbps = 256000,
    BaudRate1Mbps   = 1000000,
    BaudRate1p5Mbps = 1500000
  } baud_rate_e;
  typedef enum int {
    TxWatermark = 0, RxWatermark = 1, TxDone = 2, RxOverflow = 3, RxFrameErr = 4,
    RxBreakErr = 5, RxTimeout = 6, RxParityErr = 7, TxEmpty = 8, NumUartIntr = 9
  } uart_intr_e;
  `define CALC_NCO(baud_rate, nco_width, clk_freq_mhz) \
    (baud_rate == BaudRate1p5Mbps && clk_freq_mhz == 24) ? 16'hffff : \
        (longint'(baud_rate) * (2**(nco_width+4))) / (clk_freq_mhz * 1000_000)
endpackage
import p::*;
class uart_base;
  // p_sequencer.cfg.clk_freq_mhz and p_sequencer.cfg.ral.ctrl.nco.get_n_bits()
  uint clk_freq_mhz = 24;
  uint nco_n_bits = 16;

  // ---- uart_item
  rand bit start_bit = 1'b0;
  rand bit stop_bit = 1'b1;
  rand bit [7:0] data;
  rand bit parity;
  rand bit ovrd_en_parity = 1'b0;
  constraint start_bit_c {
    start_bit == 1'b0;
  }
  constraint stop_bit_c {
    stop_bit == 1'b1;
  }
  constraint ovrd_en_parity_c {
    ovrd_en_parity == 1'b0;
  }

  // ---- uart_base_vseq
  rand baud_rate_e baud_rate; // set baud rate
  rand bit en_tx;             // enable tx
  rand bit en_rx;             // enable rx
  rand bit en_parity;         // enable parity
  rand bit odd_parity;        // enable odd parity
  rand bit en_noise_filter;   // enable noise filter
  rand uint uart_period_glitch_pct;
  rand bit [NumUartIntr-1:0] en_intr;
  rand uint dly_to_access_fifo;

  constraint uart_period_glitch_pct_c {
    uart_period_glitch_pct inside {[0:10]};
  }

  constraint baud_rate_c {
    if (baud_rate == BaudRate1p5Mbps && clk_freq_mhz < 48) {
      en_noise_filter == 0;
      uart_period_glitch_pct == 0;
    }
    `CALC_NCO(baud_rate, nco_n_bits, clk_freq_mhz) < 2 ** nco_n_bits;
  }

  constraint dly_to_access_fifo_c {
    dly_to_access_fifo dist {
      0                   :/ 1,
      [1      :100]       :/ 1,
      [101    :10_000]    :/ 8,
      [10_001 :1_000_000] :/ 1
    };
  }
endclass
module top;
  initial begin
    uart_base t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d %0d %h %b%b%b%b%b%b%b%b %0d %h %0d", i, t.baud_rate, t.data, t.start_bit, t.stop_bit,
               t.parity, t.ovrd_en_parity, t.en_tx, t.en_rx, t.en_parity, t.en_noise_filter,
               t.uart_period_glitch_pct, t.en_intr, t.dly_to_access_fifo);
    end
    $finish;
  end
endmodule
