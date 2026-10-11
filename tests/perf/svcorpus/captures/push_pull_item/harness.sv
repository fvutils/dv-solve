// OpenTitan hw/dv/sv/push_pull_agent/push_pull_item.sv, all constraint blocks
// Parameters as the cip_base EDN agent instantiates it:
// push_pull_item #(.DeviceDataWidth(EDN_DATA_WIDTH)), EDN_DATA_WIDTH = 33.
package p;
  parameter int unsigned EDN_DATA_WIDTH = 32 + 1;
endpackage
import p::*;
class push_pull_item #(parameter int HostDataWidth = 32,
                       parameter int DeviceDataWidth = HostDataWidth);
  rand bit [HostDataWidth-1:0]      h_data;
  rand bit [DeviceDataWidth-1:0]    d_data;
  rand int unsigned host_delay;
  constraint host_delay_max_c {
    soft host_delay <= 1000;
  }
  rand int unsigned device_delay;
  constraint device_delay_max_c  {
    soft device_delay <= 1000;
  }
  rand int unsigned req_lo_delay;
  constraint req_lo_delay_max_c {
    soft req_lo_delay <= 1000;
  }
  rand int unsigned ack_lo_delay;
  constraint ack_lo_delay_max_c {
    soft ack_lo_delay <= 1000;
  }
endclass
module top;
  initial begin
    push_pull_item #(.DeviceDataWidth(EDN_DATA_WIDTH)) t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d %h %h %0d %0d %0d %0d", i, t.h_data, t.d_data, t.host_delay, t.device_delay, t.req_lo_delay, t.ack_lo_delay);
    end
    $finish;
  end
endmodule
