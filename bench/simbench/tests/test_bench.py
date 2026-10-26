"""Benchmark matrix: every config x every bench, marked by bench tier."""
import os

import pytest

from dv_solve_simbench import records
from dv_solve_simbench.config import load_configs
from dv_solve_simbench.runner import build, find_benches, run

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIGS = load_configs()
BENCHES = find_benches(os.path.join(ROOT, "benchmarks"))

N = int(os.environ.get("SIMBENCH_N", "200"))
SEED = int(os.environ.get("SIMBENCH_SEED", "1"))


def _params():
    for cfg in CONFIGS.values():
        for bench in BENCHES:
            marks = [getattr(pytest.mark, bench.tier)]
            if not cfg.available:
                marks.append(pytest.mark.skip(reason="%s: %s" % (cfg.id, cfg.reason)))
            yield pytest.param(cfg, bench, marks=marks, id="%s-%s" % (cfg.id, bench.id))


@pytest.fixture(scope="session")
def build_cache(tmp_path_factory):
    return str(tmp_path_factory.mktemp("build"))


@pytest.mark.parametrize("cfg,bench", list(_params()))
def test_bench(cfg, bench, build_cache, tmp_path, simbench_out):
    params = {"n": N, "seed": SEED}
    load0 = records._loadavg()
    try:
        exe = build(cfg, bench, build_cache)
        out = run(cfg, exe, str(tmp_path),
                  ["+sb_n=%d" % N, "+sb_seed=%d" % SEED,
                   "+sb_dump=%s" % (tmp_path / "dump.txt")])
    except Exception as e:
        if simbench_out:
            records.append(simbench_out, records.make_record(
                cfg, bench, None, params, load0, error="%s: %s" % (type(e).__name__, e)))
        raise
    if simbench_out:
        records.append(simbench_out, records.make_record(
            cfg, bench, out.result, params, load0))
    assert out.result is not None, "no SIMBENCH_RESULT (rc=%d):\n%s" % (
        out.returncode, out.log[-4000:])
    assert out.result.ok, "bench failed: %s" % out.result
