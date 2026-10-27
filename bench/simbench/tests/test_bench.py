"""Benchmark matrix: every config x every bench, marked by bench tier."""
import os

import pytest

from dv_solve_simbench import metric, records, standalone
from dv_solve_simbench.config import load_configs
from dv_solve_simbench.runner import build, find_benches, probe_sampler, run

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIGS = load_configs()
BENCHES = find_benches(os.path.join(ROOT, "benchmarks"))

N = int(os.environ.get("SIMBENCH_N", "200"))
SEED = int(os.environ.get("SIMBENCH_SEED", "1"))
# Randomization metric: dumps are kept in SIMBENCH_DUMP_DIR/<cfg>/<bench>.txt when set;
# SIMBENCH_REF_CONFIG (default vlt-z3-default) names the reference for chi2 there.
# SIMBENCH_DIVERSITY=1 also runs seeds 1..20 with n=1 for first-value diversity.
DUMP_DIR = os.environ.get("SIMBENCH_DUMP_DIR")
REF_CFG = os.environ.get("SIMBENCH_REF_CONFIG", "vlt-z3-default")
DIVERSITY = os.environ.get("SIMBENCH_DIVERSITY") == "1"


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
        if cfg.version:
            ver = records.tool_version(cfg.tool) or ""
            if cfg.version not in ver:
                raise RuntimeError("%s: %s reports %r; config wants %r"
                                   % (cfg.id, cfg.tool, ver, cfg.version))
        exe = build(cfg, bench, build_cache)
        plusargs = ["+sb_n=%d" % N, "+sb_seed=%d" % SEED,
                    "+sb_dump=%s" % (tmp_path / "dump.txt")]
        session = str(tmp_path / "session.smt2")
        if cfg.replay or cfg.replay_cmd:
            plusargs.append(standalone.session_plusarg(session))
        out = run(cfg, exe, str(tmp_path), plusargs)
        if cfg.replay_cmd:
            out = standalone.replay_text(cfg, out, session, str(tmp_path))
        elif cfg.replay:
            out = standalone.replay(cfg, out, session, str(tmp_path))
        active = probe_sampler(cfg, exe, str(tmp_path / "probe"))
        if cfg.sampler and active != cfg.sampler:
            raise RuntimeError("%s: image ran sampler %r; config wants %r"
                               % (cfg.id, active, cfg.sampler))
    except Exception as e:
        if simbench_out:
            records.append(simbench_out, records.make_record(
                cfg, bench, None, params, load0, error="%s: %s" % (type(e).__name__, e)))
        raise
    rand = _randomization(cfg, bench, exe, tmp_path)
    if simbench_out:
        records.append(simbench_out, records.make_record(
            cfg, bench, out.result, params, load0, sampler_active=active,
            randomization=rand))
    assert out.result is not None, "no SIMBENCH_RESULT (rc=%d):\n%s" % (
        out.returncode, out.log[-4000:])
    assert out.result.ok, "bench failed: %s" % out.result


def _dump_path(cfg_id, bench):
    return os.path.join(DUMP_DIR, cfg_id, bench.id.replace("/", "_") + ".txt")


def _randomization(cfg, bench, exe, tmp_path):
    dump = tmp_path / "dump.txt"
    if not dump.is_file():
        return None
    rows = metric.load_dump(str(dump))
    ref = None
    if DUMP_DIR:
        os.makedirs(os.path.dirname(_dump_path(cfg.id, bench)), exist_ok=True)
        with open(_dump_path(cfg.id, bench), "w") as fp:
            fp.write(dump.read_text())
        if cfg.id != REF_CFG and os.path.isfile(_dump_path(REF_CFG, bench)):
            ref = metric.load_dump(_dump_path(REF_CFG, bench))
    rand = metric.field_metrics(rows, ref)
    if DIVERSITY:
        first = []
        for seed in range(1, 21):
            d = tmp_path / ("first%d.txt" % seed)
            run(cfg, exe, str(tmp_path), ["+sb_n=1", "+sb_seed=%d" % seed, "+sb_dump=%s" % d])
            if d.is_file():
                first += metric.load_dump(str(d))[:1]
        rand["first_value_diversity"] = metric.first_value_diversity(first)
    return rand
