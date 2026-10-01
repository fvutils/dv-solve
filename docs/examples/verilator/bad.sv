class Item;
  rand bit [7:0] x, y;
  constraint c1 { x > 10; }
  constraint c2 { y < x; }
  constraint c3 { x < 5; }
endclass

module top;
  initial begin
    automatic Item it = new;
    if (it.randomize() == 0) $display("randomize() failed");
    $finish;
  end
endmodule
