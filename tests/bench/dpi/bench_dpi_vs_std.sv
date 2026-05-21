// Benchmark: DPI solver vs std::randomize()

import "DPI-C" function longint bench_time_ns();
import zsp_dpi_pkg::*;

// ----------------------------------------------------------------
// Classes with constraints (for std::randomize)
// ----------------------------------------------------------------

class small_cls;
  rand bit [7:0] v0, v1, v2;
  constraint ordering { v0 <= v1; v1 <= v2; }
endclass

class medium_cls;
  rand bit [31:0] v0, v1, v2, v3, v4, v5, v6, v7, v8, v9;
  constraint ordering {
    v0 <= v1; v1 <= v2; v2 <= v3; v3 <= v4;
    v4 <= v5; v5 <= v6; v6 <= v7; v7 <= v8; v8 <= v9;
  }
endclass

class large_cls;
  rand bit [15:0] v0, v1, v2, v3, v4, v5, v6, v7, v8, v9;
  rand bit [15:0] v10, v11, v12, v13, v14, v15, v16, v17, v18, v19;
  constraint ordering {
    v0<=v1; v1<=v2; v2<=v3; v3<=v4; v4<=v5; v5<=v6; v6<=v7;
    v7<=v8; v8<=v9; v9<=v10; v10<=v11; v11<=v12; v12<=v13;
    v13<=v14; v14<=v15; v15<=v16; v16<=v17; v17<=v18; v18<=v19;
  }
endclass

// ----------------------------------------------------------------
module bench_dpi_vs_std;

  int N;
  longint t0, t1;
  real dpi_us, std_us;

  task bench_small();
    chandle ctx;
    int rc;
    ctx = zsp_dpi_compile_b64("AwAAAAIAAAAAAAAAUAAAALgAAAD/////AAAAAAAAAACwAAAAsAAAAAAAAAAAAAAA/////wAAAAAIAAAAAAAAAAAAAAAAAAAA/wAAAAAAAAAQAAAAAQAAAAgAAAAAAAAAAAAAAAAAAAD/AAAAAAAAADAAAAACAAAACAAAAAAAAAAAAAAAAAAAAP8AAAAAAAAAAQAAAAAAAAABAAAAAQAAAAIAAAANAAAAcAAAAHgAAAD/////gAAAAAEAAAABAAAAAQAAAAIAAAACAAAADQAAAJgAAACgAAAAkAAAAKgAAAA=");
    if (ctx == null) $fatal(1, "small: compile failed");

    t0 = bench_time_ns();
    repeat (N) begin
      rc = zsp_dpi_solve_h(ctx, $urandom());
      if (rc != 0) $fatal(1, "small DPI solve failed");
    end
    t1 = bench_time_ns();
    dpi_us = real'(t1 - t0) / 1000.0;
    zsp_dpi_release_h(ctx);

    begin
      automatic small_cls obj = new();
      t0 = bench_time_ns();
      repeat (N) begin
        if (!obj.randomize()) $fatal(1, "small std::randomize failed");
      end
      t1 = bench_time_ns();
      std_us = real'(t1 - t0) / 1000.0;
    end

    $display("BENCH small  %0d vars  N=%0d  dpi=%.1f us  std=%.1f us  dpi/call=%.2f us  std/call=%.2f us  speedup=%.1fx",
             3, N, dpi_us, std_us, dpi_us/N, std_us/N,
             (dpi_us == 0) ? 0.0 : std_us / dpi_us);
  endtask

  task bench_medium();
    chandle ctx;
    int rc;
    ctx = zsp_dpi_compile_b64("CgAAAAkAAAAAAAAAMAEAALACAAD/////AAAAAAAAAACoAgAAqAIAAAAAAAAAAAAA/////wAAAAAgAAAAAAAAAAAAAAAAAAAA6AMAAAAAAAAQAAAAAQAAACAAAAAAAAAAAAAAAAAAAADoAwAAAAAAADAAAAACAAAAIAAAAAAAAAAAAAAAAAAAAOgDAAAAAAAAUAAAAAMAAAAgAAAAAAAAAAAAAAAAAAAA6AMAAAAAAABwAAAABAAAACAAAAAAAAAAAAAAAAAAAADoAwAAAAAAAJAAAAAFAAAAIAAAAAAAAAAAAAAAAAAAAOgDAAAAAAAAsAAAAAYAAAAgAAAAAAAAAAAAAAAAAAAA6AMAAAAAAADQAAAABwAAACAAAAAAAAAAAAAAAAAAAADoAwAAAAAAAPAAAAAIAAAAIAAAAAAAAAAAAAAAAAAAAOgDAAAAAAAAEAEAAAkAAAAgAAAAAAAAAAAAAAAAAAAA6AMAAAAAAAABAAAAAAAAAAEAAAABAAAAAgAAAA0AAABQAQAAWAEAAP////9gAQAAAQAAAAEAAAABAAAAAgAAAAIAAAANAAAAeAEAAIABAABwAQAAiAEAAAEAAAACAAAAAQAAAAMAAAACAAAADQAAAKABAACoAQAAmAEAALABAAABAAAAAwAAAAEAAAAEAAAAAgAAAA0AAADIAQAA0AEAAMABAADYAQAAAQAAAAQAAAABAAAABQAAAAIAAAANAAAA8AEAAPgBAADoAQAAAAIAAAEAAAAFAAAAAQAAAAYAAAACAAAADQAAABgCAAAgAgAAEAIAACgCAAABAAAABgAAAAEAAAAHAAAAAgAAAA0AAABAAgAASAIAADgCAABQAgAAAQAAAAcAAAABAAAACAAAAAIAAAANAAAAaAIAAHACAABgAgAAeAIAAAEAAAAIAAAAAQAAAAkAAAACAAAADQAAAJACAACYAgAAiAIAAKACAAA=");
    if (ctx == null) $fatal(1, "medium: compile failed");

    t0 = bench_time_ns();
    repeat (N) begin
      rc = zsp_dpi_solve_h(ctx, $urandom());
      if (rc != 0) $fatal(1, "medium DPI solve failed");
    end
    t1 = bench_time_ns();
    dpi_us = real'(t1 - t0) / 1000.0;
    zsp_dpi_release_h(ctx);

    begin
      automatic medium_cls obj = new();
      t0 = bench_time_ns();
      repeat (N) begin
        if (!obj.randomize()) $fatal(1, "medium std::randomize failed");
      end
      t1 = bench_time_ns();
      std_us = real'(t1 - t0) / 1000.0;
    end

    $display("BENCH medium %0d vars  N=%0d  dpi=%.1f us  std=%.1f us  dpi/call=%.2f us  std/call=%.2f us  speedup=%.1fx",
             10, N, dpi_us, std_us, dpi_us/N, std_us/N,
             (dpi_us == 0) ? 0.0 : std_us / dpi_us);
  endtask

  task bench_large();
    chandle ctx;
    int rc;
    ctx = zsp_dpi_compile_b64("FAAAABMAAAAAAAAAcAIAAIAFAAD/////AAAAAAAAAAB4BQAAeAUAAAAAAAAAAAAA/////wAAAAAQAAAAAAAAAAAAAAAAAAAA9AEAAAAAAAAQAAAAAQAAABAAAAAAAAAAAAAAAAAAAAD0AQAAAAAAADAAAAACAAAAEAAAAAAAAAAAAAAAAAAAAPQBAAAAAAAAUAAAAAMAAAAQAAAAAAAAAAAAAAAAAAAA9AEAAAAAAABwAAAABAAAABAAAAAAAAAAAAAAAAAAAAD0AQAAAAAAAJAAAAAFAAAAEAAAAAAAAAAAAAAAAAAAAPQBAAAAAAAAsAAAAAYAAAAQAAAAAAAAAAAAAAAAAAAA9AEAAAAAAADQAAAABwAAABAAAAAAAAAAAAAAAAAAAAD0AQAAAAAAAPAAAAAIAAAAEAAAAAAAAAAAAAAAAAAAAPQBAAAAAAAAEAEAAAkAAAAQAAAAAAAAAAAAAAAAAAAA9AEAAAAAAAAwAQAACgAAABAAAAAAAAAAAAAAAAAAAAD0AQAAAAAAAFABAAALAAAAEAAAAAAAAAAAAAAAAAAAAPQBAAAAAAAAcAEAAAwAAAAQAAAAAAAAAAAAAAAAAAAA9AEAAAAAAACQAQAADQAAABAAAAAAAAAAAAAAAAAAAAD0AQAAAAAAALABAAAOAAAAEAAAAAAAAAAAAAAAAAAAAPQBAAAAAAAA0AEAAA8AAAAQAAAAAAAAAAAAAAAAAAAA9AEAAAAAAADwAQAAEAAAABAAAAAAAAAAAAAAAAAAAAD0AQAAAAAAABACAAARAAAAEAAAAAAAAAAAAAAAAAAAAPQBAAAAAAAAMAIAABIAAAAQAAAAAAAAAAAAAAAAAAAA9AEAAAAAAABQAgAAEwAAABAAAAAAAAAAAAAAAAAAAAD0AQAAAAAAAAEAAAAAAAAAAQAAAAEAAAACAAAADQAAAJACAACYAgAA/////6ACAAABAAAAAQAAAAEAAAACAAAAAgAAAA0AAAC4AgAAwAIAALACAADIAgAAAQAAAAIAAAABAAAAAwAAAAIAAAANAAAA4AIAAOgCAADYAgAA8AIAAAEAAAADAAAAAQAAAAQAAAACAAAADQAAAAgDAAAQAwAAAAMAABgDAAABAAAABAAAAAEAAAAFAAAAAgAAAA0AAAAwAwAAOAMAACgDAABAAwAAAQAAAAUAAAABAAAABgAAAAIAAAANAAAAWAMAAGADAABQAwAAaAMAAAEAAAAGAAAAAQAAAAcAAAACAAAADQAAAIADAACIAwAAeAMAAJADAAABAAAABwAAAAEAAAAIAAAAAgAAAA0AAACoAwAAsAMAAKADAAC4AwAAAQAAAAgAAAABAAAACQAAAAIAAAANAAAA0AMAANgDAADIAwAA4AMAAAEAAAAJAAAAAQAAAAoAAAACAAAADQAAAPgDAAAABAAA8AMAAAgEAAABAAAACgAAAAEAAAALAAAAAgAAAA0AAAAgBAAAKAQAABgEAAAwBAAAAQAAAAsAAAABAAAADAAAAAIAAAANAAAASAQAAFAEAABABAAAWAQAAAEAAAAMAAAAAQAAAA0AAAACAAAADQAAAHAEAAB4BAAAaAQAAIAEAAABAAAADQAAAAEAAAAOAAAAAgAAAA0AAACYBAAAoAQAAJAEAACoBAAAAQAAAA4AAAABAAAADwAAAAIAAAANAAAAwAQAAMgEAAC4BAAA0AQAAAEAAAAPAAAAAQAAABAAAAACAAAADQAAAOgEAADwBAAA4AQAAPgEAAABAAAAEAAAAAEAAAARAAAAAgAAAA0AAAAQBQAAGAUAAAgFAAAgBQAAAQAAABEAAAABAAAAEgAAAAIAAAANAAAAOAUAAEAFAAAwBQAASAUAAAEAAAASAAAAAQAAABMAAAACAAAADQAAAGAFAABoBQAAWAUAAHAFAAA=");
    if (ctx == null) $fatal(1, "large: compile failed");

    t0 = bench_time_ns();
    repeat (N) begin
      rc = zsp_dpi_solve_h(ctx, $urandom());
      if (rc != 0) $fatal(1, "large DPI solve failed");
    end
    t1 = bench_time_ns();
    dpi_us = real'(t1 - t0) / 1000.0;
    zsp_dpi_release_h(ctx);

    begin
      automatic large_cls obj = new();
      t0 = bench_time_ns();
      repeat (N) begin
        if (!obj.randomize()) $fatal(1, "large std::randomize failed");
      end
      t1 = bench_time_ns();
      std_us = real'(t1 - t0) / 1000.0;
    end

    $display("BENCH large  %0d vars  N=%0d  dpi=%.1f us  std=%.1f us  dpi/call=%.2f us  std/call=%.2f us  speedup=%.1fx",
             20, N, dpi_us, std_us, dpi_us/N, std_us/N,
             (dpi_us == 0) ? 0.0 : std_us / dpi_us);
  endtask

  initial begin
    if (!$value$plusargs("N=%d", N))
      N = 10000;

    $display("=== DPI vs std::randomize benchmark (N=%0d) ===", N);
    bench_small();
    bench_medium();
    bench_large();
    $display("=== Done ===");
    $finish;
  end

endmodule
