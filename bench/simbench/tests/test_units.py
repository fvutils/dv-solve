"""Unit tests for config loading and result parsing (no simulator needed)."""
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
    assert list(cfgs) == ["vlt-z3", "vlt-dvs", "vlt-dvs-native", "vlt540-z3"]
    c = cfgs["vlt-dvs-native"]
    assert c.run_env()["VERILATOR_SOLVER"].endswith("dv-solve-smt2")
    assert c.run_plusargs() == ["+verilator+rand+sampler+solver"]
    assert cfgs["vlt540-z3"].run_plusargs() == []
    assert cfgs["vlt-z3"].run_env() == {"VERILATOR_SOLVER": "z3 --in"}


def test_missing_tool_unavailable(tmp_path):
    f = tmp_path / "local.yaml"
    f.write_text("configs:\n"
                 "  - id: vlt-z3\n    tool: /nonexistent/verilator\n    solver: z3 --in\n"
                 "  - id: nosolver\n    tool: ${SH}\n    solver: /nonexistent/solver\n"
                 "  - id: ok\n    tool: ${SH}\n    solver: ${SH} -c x\n    sampler: solver\n")
    env = {"SIMBENCH_CONFIGS": str(f), "SH": "/bin/sh"}
    cfgs = load_configs(environ=env)
    assert not cfgs["vlt-z3"].available and "simulator not found" in cfgs["vlt-z3"].reason
    assert not cfgs["nosolver"].available and "solver not found" in cfgs["nosolver"].reason
    assert cfgs["ok"].available, cfgs["ok"].reason
    # Local file overrides by id but keeps the rest of the defaults
    assert "vlt-dvs" in cfgs


def test_detect_here():
    """Not an assertion on tools: every config gets a definite status."""
    for c in load_configs().values():
        assert c.available or c.reason
