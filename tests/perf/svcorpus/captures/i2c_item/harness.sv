// OpenTitan hw/dv/sv/i2c_agent/i2c_item.sv, fbyte_c + rcont_c + wait_cycles_c
class i2c_item;
  // Use to indicate the number of cycles Agent consumes for Write data or while in idle state
  int                      wait_cycles = 8;
  rand bit [7:0]           fbyte;
  rand bit                 nakok, rcont, read, stop, start;

  constraint fbyte_c     { fbyte      inside {[0 : 127] }; }
  constraint rcont_c     {
     solve read, stop before rcont;
     // for read request, rcont and stop must be complementary set
     if (read) {
       rcont == ~stop;
     } else {
       rcont dist { 1 :/ 1, 0 :/ 2 };
     }
  }

  constraint wait_cycles_c {
    wait_cycles == 8;
  }
endclass
module top;
  initial begin
    i2c_item t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d %h %b%b%b%b%b", i, t.fbyte, t.nakok, t.rcont, t.read, t.stop, t.start);
    end
    $finish;
  end
endmodule
