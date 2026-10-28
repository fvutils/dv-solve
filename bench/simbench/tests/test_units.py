"""Unit tests for config loading and result parsing (no simulator needed)."""
import math
import os

import pytest

from dv_solve_simbench.config import expand, load_configs
from dv_solve_simbench.result import parse_result

LINE = ('SIMBENCH_RESULT {"bench":"construct.range","n":200,"n_req":200,"warmup":10,'
        '"failures":0,"check_errors":0,"elapsed_ns":89000000,"first_call_ns":10800000,'
        '"timed_out":0}')


def test_parse_result():
    out = "- V e r i l a t i o n\nsome log\n" + LINE + "\n- simbench_top.sv:10: Verilog $finish\n"
    r = parse_result(out)
    assert r.bench == "construct.range"
    assert r.n == 200 and r.elapsed_ns == 89000000
    assert r.timed_out is False and r.ok
    assert r.ns_per_call == pytest.approx(445000.0)


def test_parse_result_absent_and_bad():
    assert parse_result("no result here\n") is None
    with pytest.raises(ValueError):
        parse_result("SIMBENCH_RESULT {not json")
    with pytest.raises(ValueError):
        parse_result('SIMBENCH_RESULT {"bench":"x"}')


def test_parse_result_not_ok():
    r = parse_result(LINE.replace('"failures":0', '"failures":3').replace('"n":200', '"n":150'))
    assert r.failures == 3 and not r.ok


def test_expand():
    env = {"A": "/x", "HOME": os.environ.get("HOME", "/h")}
    assert expand("${A}/bin", env) == "/x/bin"
    assert expand("${B:-/d}/bin", env) == "/d/bin"
    assert expand("${A:-/d} --in", env) == "/x --in"
    assert expand("~/v", env) == os.path.expanduser("~/v")


def test_default_configs():
    cfgs = load_configs(environ={}, detect=False)
    assert list(cfgs) == ["vlt-z3-default", "vlt-dvs-solver", "dvs-standalone",
                          "z3-standalone", "dvs-smt2-standalone", "vlt-dvs-default", "vlt540-z3",
                          "vcs", "xcelium", "questa"]
    assert cfgs["dvs-standalone"].replay.endswith("bench_vlt_replay")
    assert cfgs["vlt-dvs-solver"].replay is None
    assert cfgs["z3-standalone"].replay_cmd == "z3 -smt2"
    assert cfgs["dvs-smt2-standalone"].replay_cmd.endswith("dv-solve-smt2")
    assert cfgs["z3-standalone"].replay_reps == 1
    c = cfgs["vlt-dvs-solver"]
    assert c.run_env()["VERILATOR_SOLVER"].endswith("dv-solve-smt2")
    assert c.run_plusargs() == ["+verilator+rand+sampler+solver"]
    assert cfgs["vlt540-z3"].run_plusargs() == []
    assert cfgs["vlt-z3-default"].run_env() == {"VERILATOR_SOLVER": "z3 --in"}


def test_missing_tool_unavailable(tmp_path):
    f = tmp_path / "local.yaml"
    f.write_text("configs:\n"
                 "  - id: vlt-z3-default\n    tool: /nonexistent/verilator\n    solver: z3 --in\n"
                 "  - id: nosolver\n    tool: ${SH}\n    solver: /nonexistent/solver\n"
                 "  - id: ok\n    tool: ${SH}\n    solver: ${SH} -c x\n    sampler: solver\n")
    env = {"SIMBENCH_CONFIGS": str(f), "SH": "/bin/sh"}
    cfgs = load_configs(environ=env)
    assert not cfgs["vlt-z3-default"].available and "simulator not found" in cfgs["vlt-z3-default"].reason
    assert not cfgs["nosolver"].available and "solver not found" in cfgs["nosolver"].reason
    assert cfgs["ok"].available, cfgs["ok"].reason
    # Local file overrides by id but keeps the rest of the defaults
    assert "vlt-dvs-default" in cfgs


def test_detect_here():
    """Not an assertion on tools: every config gets a definite status."""
    for c in load_configs().values():
        assert c.available or c.reason


def _perf_schema():
    """dv-solve's tests/perf/schema.py when simbench runs from a dv-solve checkout."""
    import importlib.util
    path = os.path.join(os.path.dirname(__file__), "..", "..", "..", "tests", "perf", "schema.py")
    if not os.path.isfile(path):
        pytest.skip("tests/perf/schema.py not found")
    spec = importlib.util.spec_from_file_location("perf_schema", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_record_validates(tmp_path):
    from dv_solve_simbench import records
    from dv_solve_simbench.config import Config
    from dv_solve_simbench.runner import Bench
    schema = _perf_schema()
    cfg = Config(id="vlt-z3-default", simulator="verilator", tool="/nonexistent/verilator", solver="z3 --in")
    bench = Bench(id="construct.range", tier="smoke", path="bench.sv")
    ok = records.make_record(cfg, bench, parse_result(LINE), {"n": 200, "seed": 1}, 0.1)
    bad = records.make_record(cfg, bench, None, {"n": 200, "seed": 1}, 0.1, error="BuildError: x")
    assert ok["valid"] and not bad["valid"] and bad["reason"]
    out = str(tmp_path / "r.jsonl")
    records.append(out, ok)
    records.append(out, bad)
    for rec in records.load(out):
        assert schema.validate(rec) == []
    assert ok["simbench"]["result"]["ns_per_call"] == 445000.0


def test_dev_configs_want_dev_verilator():
    cfgs = load_configs(environ={}, detect=False)
    for cid in ("vlt-z3-default", "vlt-dvs-solver", "vlt-dvs-default"):
        assert cfgs[cid].version == "5.053 devel"
    assert cfgs["vlt540-z3"].version is None


def test_parse_replay():
    from dv_solve_simbench.standalone import parse_replay
    text = ("api-cache  wall   0.000 s     14.7 us/check-sat   (CPU 0.000 s; 2 distinct, "
            "29 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 1 unsolved)\n"
            "api-cache  per call      29.4 us   (20 timed calls)\n"
            "             solve+read         0.000 s     13.0 us\n"
            "               on a reused context: 12.1 us per call; on a new one: 25.9 us\n")
    assert parse_replay(text, "api-cache") == {"us_per_check": 14.7, "failures": 1, "us_new": 25.9,
                                               "us_per_call": 29.4, "timed_calls": 20}
    assert parse_replay(text, "api") is None


def test_solver_mode():
    from dv_solve_simbench.standalone import solver_mode
    assert solver_mode("/x/dv-solve-smt2") == "default"
    assert solver_mode("/x/dv-solve-smt2 --mode=verilator") == "verilator"
    assert solver_mode("/x/dv-solve-smt2 --mode=smt") == "default"
    assert solver_mode(None) == "default"


def test_metric_fields():
    from dv_solve_simbench import metric
    rows = [[str(i), str(i % 2)] for i in range(100)]
    m = metric.field_metrics(rows)
    assert m["n"] == 100
    assert m["fields"][0] == {"distinct": 1.0, "entropy": 1.0}
    assert m["fields"][1]["distinct"] == 0.02
    assert m["min_distinct"] == 0.02
    assert abs(m["fields"][1]["entropy"] - math.log(2) / math.log(100)) < 1e-3


def test_metric_chi2():
    from dv_solve_simbench import metric
    same = [["a"], ["b"]] * 50
    skew = [["a"]] * 90 + [["b"]] * 10
    assert metric.field_metrics(same, same)["max_chi2"] == 0.0
    assert metric.field_metrics(skew, same)["max_chi2"] > 20
    # all-distinct fields have no shared bins: no chi2
    wide = [[str(i)] for i in range(100)]
    assert "chi2" not in metric.field_metrics(wide, wide)["fields"][0]


def test_metric_first_value_diversity():
    from dv_solve_simbench import metric
    assert metric.first_value_diversity([["1"], ["2"], ["1"], ["3"]]) == 0.75
    assert metric.first_value_diversity([]) == 0.0


def test_record_randomization():
    from dv_solve_simbench import metric, records
    from dv_solve_simbench.config import Config
    from dv_solve_simbench.runner import Bench
    cfg = Config(id="vlt-dvs-solver", simulator="verilator", tool="/nonexistent/verilator", solver="x")
    bench = Bench(id="construct.range", tier="smoke", path="bench.sv")
    rand = metric.field_metrics([["1", "2"], ["3", "2"]])
    rec = records.make_record(cfg, bench, parse_result(LINE), {"n": 200, "seed": 1}, 0.1,
                              randomization=rand)
    assert _perf_schema().validate(rec) == []
    assert rec["simbench"]["result"]["randomization"]["min_distinct"] == 0.5


def test_count_answers():
    from dv_solve_simbench.standalone import count_answers
    a = count_answers("sat\n((x #x01))\nunsat\nsat\n(error \"bad\")\n")
    assert a == {"sat": 2, "unsat": 1, "unknown": 0, "error": 1}


def test_extract_cmds_joins_wrapped(tmp_path):
    from dv_solve_simbench.standalone import extract_cmds
    log = tmp_path / "s.smt2"
    log.write_text("< sat\n  (declare-fun w () (_ BitVec 32))\n"
                   "  (assert (= w #x0\n  000001f))\n  (check-sat)\n< sat\n")
    out = tmp_path / "c.smt2"
    assert extract_cmds(str(log), str(out)) == 1
    assert out.read_text().splitlines() == [
        "(declare-fun w () (_ BitVec 32))", "(assert (= w #x0000001f))", "(check-sat)"]


def _bench(tmp_path):
    from dv_solve_simbench.runner import Bench
    return Bench(id="g/b", tier="smoke", path=str(tmp_path / "bench.sv"))


def test_build_cmds_verilator(tmp_path):
    from dv_solve_simbench.runner import build_cmds, image_path, run_cmd
    c = load_configs(environ={}, detect=False)["vlt-z3-default"]
    c.tool = "/v/bin/verilator"
    (cmd,) = build_cmds(c, _bench(tmp_path), "/b")
    assert cmd[:7] == ["/v/bin/verilator", "--binary", "-Wno-fatal", "--top-module",
                       "simbench_top", "-Mdir", "/b"]
    assert cmd[7].endswith("simbench_pkg.sv") and cmd[-1].endswith("sb_time.c")
    assert image_path(c, "/b") == "/b/Vsimbench_top"
    assert run_cmd(c, "/b/Vsimbench_top", ["+sb_n=5"]) == [
        "/b/Vsimbench_top", "+verilator+rand+sampler+default", "+sb_n=5"]


def test_build_cmds_commercial(tmp_path):
    from dv_solve_simbench.runner import build_cmds, image_path, run_cmd
    cfgs = load_configs(environ={}, detect=False)
    b = _bench(tmp_path)
    vcs = cfgs["vcs"]
    (cmd,) = build_cmds(vcs, b, "/b")
    assert cmd[0] == "vcs" and "-sverilog" in cmd and cmd[-1].endswith("sb_time.c")
    assert cmd[cmd.index("-o") + 1] == "/b/simv"
    assert run_cmd(vcs, image_path(vcs, "/b"), ["+sb_n=5"]) == ["/b/simv", "+sb_n=5"]
    assert vcs.run_env() == {} and vcs.version_flag == "-ID"
    x = cfgs["xcelium"]
    (cmd,) = build_cmds(x, b, "/b")
    assert cmd[0] == "xrun" and "-elaborate" in cmd and cmd[cmd.index("-xmlibdirpath") + 1] == "/b"
    assert run_cmd(x, image_path(x, "/b"), ["+sb_n=5"]) == [
        "xrun", "-64bit", "-R", "-xmlibdirpath", "/b", "+sb_n=5"]
    q = cfgs["questa"]
    q.tool = "/q/bin/vsim"
    vlib, vlog = build_cmds(q, b, "/b")
    assert vlib == ["/q/bin/vlib", "/b/work"]
    assert vlog[0] == "/q/bin/vlog" and vlog[-1].endswith("sb_time.c")
    assert run_cmd(q, image_path(q, "/b"), ["+sb_n=5"]) == [
        "/q/bin/vsim", "-c", "-lib", "/b/work", "simbench_top", "-do", "run -all; quit -f", "+sb_n=5"]


def test_simulator_checks(tmp_path):
    f = tmp_path / "local.yaml"
    f.write_text("configs:\n"
                 "  - id: bad\n    simulator: iverilog\n    tool: ${SH}\n"
                 "  - id: vcs-solver\n    simulator: vcs\n    tool: ${SH}\n    solver: ${SH}\n"
                 "  - id: vcs-ok\n    simulator: vcs\n    tool: ${SH}\n    build_args: [-cm, line]\n")
    cfgs = load_configs(environ={"SIMBENCH_CONFIGS": str(f), "SH": "/bin/sh"})
    assert not cfgs["bad"].available and "unsupported simulator" in cfgs["bad"].reason
    assert not cfgs["vcs-solver"].available and "Verilator-only" in cfgs["vcs-solver"].reason
    assert cfgs["vcs-ok"].available and cfgs["vcs-ok"].build_args == ["-cm", "line"]
    from dv_solve_simbench.runner import BuildError, build_cmds
    cfgs["bad"].simulator = "iverilog"
    with pytest.raises(BuildError, match="unknown simulator"):
        build_cmds(cfgs["bad"], _bench(tmp_path), "/b")


def test_record_sharing_fields(tmp_path, monkeypatch):
    from dv_solve_simbench import records
    from dv_solve_simbench.config import Config
    from dv_solve_simbench.runner import Bench
    sv = tmp_path / "bench.sv"
    sv.write_text("module m; endmodule\n")
    monkeypatch.setenv("SIMBENCH_HOST", "box1")
    cfg = Config(id="vlt-z3-default", simulator="verilator", tool="/nonexistent/verilator", solver="z3 --in")
    rec = records.make_record(cfg, Bench(id="construct.range", tier="smoke", path=str(sv)),
                              parse_result(LINE), {"n": 200, "seed": 1}, 0.1)
    assert rec["machine"]["label"] == "box1" and rec["machine"]["cpu"]
    assert len(rec["simbench"]["bench_sha"]) == 40
    assert "dirty" in rec["run"]
    assert _perf_schema().validate(rec) == []


def _rec(cfg, bench, host, ns, valid=True):
    return {"valid": valid, "reason": "" if valid else "BuildError: x",
            "machine": {"id": "m0", "label": host, "cpu": "c", "cores": 1, "mem_gb": 1, "kernel": "6"},
            "simbench": {"config": cfg, "bench": bench, "result": {"ns_per_call": ns}}}


def test_report_table():
    from dv_solve_simbench.report import table
    recs = [_rec("A", "b1", "h1", 4000), _rec("B", "b1", "h1", 1000),
            _rec("A", "b2", "h1", 2000), _rec("B", "b2", "h1", 2000),
            _rec("B", "b2", "h1", 4000),                      # repeat: last wins
            _rec("A", "b1", "h2", 8000), _rec("B", "b1", "h2", 1, valid=False)]
    t = table(recs, ref="A")
    lines = t.splitlines()
    assert lines[0] == "| bench | A@h1 | A@h2 | B@h1 | B@h2 |"
    assert lines[2] == "| b1 | 4.0 | 8.0 | 1.0 (0.25) | FAIL: BuildError: x |"
    assert lines[3] == "| b2 | 2.0 | - | 4.0 (2.00) | - |"
    assert "0.707 (n=2)" in lines[4]          # geomean of 0.25 and 2.0
