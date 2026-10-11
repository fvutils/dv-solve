// OpenTitan hw/dv/sv/usb20_agent/usb20_item.sv: data_pkt (the only usb20_item subclass with a
// constraint block), data_c
class data_pkt;
  rand byte unsigned data[]; // Dynamic array
  constraint data_c {
    data.size() <= 64;
  }
endclass
module top;
  initial begin
    data_pkt t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d %0d %p", i, t.data.size(), t.data);
    end
    $finish;
  end
endmodule
