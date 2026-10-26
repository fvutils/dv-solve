// simbench harness: base class for one benchmark.
//
// Each bench.sv defines `class sb_bench_impl extends simbench_pkg::sb_bench`
// (one bench per compile); simbench_top.sv instantiates it and calls run().
package simbench_pkg;

  import "DPI-C" function longint sb_time_ns();

  virtual class sb_bench;
    string name;

    function new(string name);
      this.name = name;
    endfunction

    // Randomize the bench object(s) once; return 1 on success.
    pure virtual function bit do_randomize();

    // Procedurally re-check the constraints; return the number of violations.
    virtual function int check();
      return 0;
    endfunction

    // Write the current solution as one line to fd.
    virtual function void dump(int fd);
    endfunction

    // Called once before warm-up, after the seed has been applied.
    virtual function void setup();
    endfunction

    task run();
      int n = 1000;
      int warmup = 10;
      int seed;
      string dump_file;
      longint timeout_ns = 0;
      int fd = 0;
      int failures = 0;
      int check_errors = 0;
      int done = 0;
      process proc;
      longint t0, t1, t_end, first_call_ns, elapsed_ns;

      void'($value$plusargs("sb_n=%d", n));
      void'($value$plusargs("sb_warmup=%d", warmup));
      void'($value$plusargs("sb_timeout_ns=%d", timeout_ns));
      if ($value$plusargs("sb_seed=%d", seed)) begin
        proc = process::self();
        proc.srandom(seed);
      end
      if ($value$plusargs("sb_dump=%s", dump_file)) begin
        fd = $fopen(dump_file, "w");
        if (fd == 0) $display("SIMBENCH_ERROR cannot open dump file %s", dump_file);
      end

      setup();

      // First call is timed apart (solver start-up, declarations).
      t0 = sb_time_ns();
      if (!do_randomize()) failures++;
      else check_errors += check();
      first_call_ns = sb_time_ns() - t0;

      for (int i = 1; i < warmup; i++) begin
        if (!do_randomize()) failures++;
        else check_errors += check();
      end

      t0 = sb_time_ns();
      for (int i = 0; i < n; i++) begin
        if (!do_randomize()) failures++;
        else begin
          check_errors += check();
          if (fd != 0) dump(fd);
        end
        done++;
        if (timeout_ns > 0 && (sb_time_ns() - t0) > timeout_ns) break;
      end
      t1 = sb_time_ns();
      elapsed_ns = t1 - t0;

      if (fd != 0) $fclose(fd);

      $display("SIMBENCH_RESULT {\"bench\":\"%s\",\"n\":%0d,\"n_req\":%0d,\"warmup\":%0d,\"failures\":%0d,\"check_errors\":%0d,\"elapsed_ns\":%0d,\"first_call_ns\":%0d,\"timed_out\":%0d}",
               name, done, n, warmup, failures, check_errors, elapsed_ns, first_call_ns,
               (done < n) ? 1 : 0);
    endtask
  endclass

endpackage
