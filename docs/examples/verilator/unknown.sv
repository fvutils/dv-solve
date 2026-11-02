class Item;
  rand bit [65599:0] x;               // wider than dv-solve's 65535-bit limit
  constraint c { x[65599] == 1'b1; }
endclass

module top;
  initial begin
    automatic Item it = new;
    if (it.randomize() == 0) $display("randomize() failed");
    $finish;
  end
endmodule
