// OpenTitan hw/dv/sv/spi_agent/spi_item.sv, all constraint blocks
package p;
  typedef bit [31:0] uint;
  parameter uint CSB_WIDTH = 2;
  typedef enum {
    SpiTransNormal,
    SpiTransSckNoCsb,
    SpiTransCsbNoSck,
    SpiTransIncompleteOpcode
  } spi_trans_type_e;
endpackage
import p::*;
class spi_item;
  rand spi_trans_type_e item_type;
  rand logic [7:0] data[$];
  bit first_byte;
  rand int read_size;
  rand logic [7:0] payload_q[$];
  rand bit write_command;
  rand bit [7:0] address_q[$];
  rand bit [7:0] opcode;
  rand bit [2:0] num_lanes;
  rand int dummy_cycles;
  rand int read_pipeline_mode;
  rand bit terminated_before_dummy_cycles;
  rand bit terminated_before_read_pipeline;
  rand uint dummy_sck_cnt;
  rand uint dummy_csb_length_ns;
  rand bit [CSB_WIDTH-1:0] csb_sel;
  rand bit [3:0]           plain_data_q[$];

  constraint data_size_c { data.size() inside {[0:65536]}; }

  constraint dummy_sck_cnt_c {
    if (item_type == SpiTransSckNoCsb) {
      dummy_sck_cnt inside {[1:1000]};
    } else {
      dummy_sck_cnt == 0;
    }}

  constraint dummy_csb_length_ns_c {
    if (item_type == SpiTransCsbNoSck) {
      dummy_csb_length_ns inside {[1:1000]};
    } else {
      dummy_csb_length_ns == 0;
    }}

  constraint num_lanes_c {
    write_command -> num_lanes == 1;
    num_lanes inside {0, 1, 2, 4};
  }
endclass
module top;
  initial begin
    spi_item t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d type=%0d sck=%0d csb=%0d wc=%0d lanes=%0d dsize=%0d", i, t.item_type, t.dummy_sck_cnt, t.dummy_csb_length_ns, t.write_command, t.num_lanes, t.data.size());
    end
    $finish;
  end
endmodule
