// simbench top: compile after simbench_pkg.sv and one bench.sv
// that defines class sb_bench_impl.
module simbench_top;
  initial begin
    sb_bench_impl b = new();
    b.run();
    $finish;
  end
endmodule
