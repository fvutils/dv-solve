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
                          "z3-standalone", "dvs-smt2-standalone", "vlt-dvs-default", "vlt540-z3"]
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
            "             solve+read         0.000 s     13.0 us\n"
            "               on a reused context: 12.1 us per call; on a new one: 25.9 us\n")
    assert parse_replay(text, "api-cache") == {"us_per_check": 14.7, "failures": 1, "us_new": 25.9}
    assert parse_replay(text, "api") is None


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
