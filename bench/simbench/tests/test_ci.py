"""The CI driver's checks and its run record (no simulator needed)."""
import pytest

from dv_solve_simbench import ci


def _rec(bench="g.b", valid=True, config="vlt-dvs-solver", simulator="verilator", reason=None,
         ns=10000.0, failures=0, check_errors=0, rand=None, result=True):
    res = None
    if result:
        res = {"ns_per_call": ns, "first_call_ns": 5e5, "failures": failures,
               "check_errors": check_errors}
        if rand is not None:
            res["randomization"] = rand
    r = {"valid": valid, "simbench": {"config": config, "bench": bench, "bench_sha": "s" + bench,
                                      "result": res},
         "tools": {"simulator": simulator}}
    if reason:
        r["reason"] = reason
    return r


def test_outcome():
    assert ci.outcome(_rec()) == "ok"
    assert ci.outcome(_rec(valid=False, check_errors=3)) == "wrong"
    assert ci.outcome(_rec(valid=False, failures=210)) == "failed"
    assert ci.outcome(_rec(valid=False, simulator="xezim", result=False,
                           reason="BuildError: build failed (1)")) == "unsupported"
    # A Verilator build failure is a broken harness, not an unsupported bench.
    assert ci.outcome(_rec(valid=False, result=False, reason="BuildError: x")) == "error"


def test_check_build():
    good = {"build_info": "x", "optimized": 1, "asan": 0, "binary": "/b/build/dv-solve-smt2",
            "library": "/b/build/lib/libdv_solve.so"}
    assert ci.check_build(good, "smt2") is good
    static = dict(good, library="/b/build/dv-solve-smt2")      # links it statically
    assert ci.check_build(static, "smt2") is static
    with pytest.raises(ci.Check, match="not Release"):
        ci.check_build(dict(good, optimized=0), "smt2")
    with pytest.raises(ci.Check, match="not its own"):
        ci.check_build(dict(good, library="/old/build/lib/libdv_solve.so"), "smt2")
    with pytest.raises(ci.Check, match="no --build-info"):
        ci.check_build(None, "smt2")


def test_check_calib():
    ci.check_calib({"cpu_ns_per_iter": 1.05, "dvs_solve_us": 0.72})
    ci.check_calib({"cpu_ns_per_iter": 2.0, "dvs_solve_us": 1.5})      # slower host: fine
    with pytest.raises(ci.Check, match="build is slow"):
        ci.check_calib({"cpu_ns_per_iter": 1.05, "dvs_solve_us": 2.0})


def _pf(bench, config, ns, ent, stuck=0, invalid=0):
    if config == "dvs-standalone":          # the replay's own answers
        rand = {"source": "capture", "min_entropy": 0.0,
                "replay": {"models": 211, "invalid": invalid, "stuck": stuck, "min_entropy": ent}}
    else:
        rand = {"n": 200, "min_entropy": ent, "fields": []}
    return _rec(bench, config=config, ns=ns, rand=rand)


def _preflight(change=None):
    change = change or {}
    recs = []
    for (config, bench), (us, ent) in sorted(ci.PREFLIGHT_REF.items()):
        kw = change.get((config, bench), {})
        recs.append(_pf(bench, config, kw.get("ns", us * 2e3), kw.get("ent", ent),
                        kw.get("stuck", 0), kw.get("invalid", 0)))
    return recs


def test_check_preflight():
    calib = {"dvs_solve_us": 0.72 * 2}           # a host half as fast: twice the reference time
    out = ci.check_preflight(_preflight(), calib)
    assert out["dvs-standalone/memmap.soc_mem_map"] == {"us": 1004.0, "min_entropy": 0.258}
    mm = ("dvs-standalone", "memmap.soc_mem_map")
    with pytest.raises(ci.Check, match="x3 allowed"):
        ci.check_preflight(_preflight({mm: {"ns": 502e3 * 2 * 3.1}}), calib)
    with pytest.raises(ci.Check, match="want 12"):
        ci.check_preflight(_preflight()[:5], calib)
    with pytest.raises(ci.Check, match="failed"):
        ci.check_preflight([_rec("x", valid=False, reason="r")] + _preflight()[1:], calib)


def test_check_preflight_spread():
    """The 2026-10-28 build was fast because its sampler never moved: a stuck
    field, or spread well under the reference, fails the pre-flight."""
    calib = {"dvs_solve_us": 0.72}
    mm = ("dvs-standalone", "memmap.soc_mem_map")
    with pytest.raises(ci.Check, match="1 stuck"):
        ci.check_preflight(_preflight({mm: {"ns": 35e3, "stuck": 1}}), calib)
    with pytest.raises(ci.Check, match="invalid model"):
        ci.check_preflight(_preflight({mm: {"invalid": 2}}), calib)
    vb = ("vlt-dvs-solver", "scenario.bus_transaction")
    with pytest.raises(ci.Check, match="min entropy 0.0"):
        ci.check_preflight(_preflight({vb: {"ent": 0.0}}), calib)
    ci.check_preflight(_preflight({vb: {"ent": 0.261 - 0.04}}), calib)    # within the margin


def test_variant_block_and_spread():
    xz = {"simulator": "xezim", "version": "0.11.3", "git": "058f2bba",
          "build_info": {"xezim-core": {"sha": "48820a89e113" + "0" * 28}}}
    rand = {"n": 200, "min_distinct": 0.45, "min_entropy": 0.82,
            "fields": [{"distinct": 0.45, "entropy": 0.82, "chi2": 1.5},
                       {"distinct": 1.0, "entropy": 1.0, "chi2": 0.9}]}
    recs = [_rec("a", simulator="xezim", config="xezim", ns=4e3, rand=rand),
            _rec("b", simulator="xezim", config="xezim", ns=16e3),
            _rec("c", simulator="xezim", config="xezim", valid=False, check_errors=2)]
    for r in recs:
        r["tools"]["simulator_build"] = xz
    blk = ci.variant_block("xezim", "xezim", recs)
    assert blk["sim"] == "xezim@0.11.3+058f2bb.48820a8"
    assert blk["dvs"] is None
    assert blk["counts"] == {"ok": 2, "wrong": 1}
    assert blk["geo_us"] == pytest.approx(8.0)
    a = blk["benches"][0]["spread"]
    assert a["min_entropy"] == 0.82 and a["chi2_max"] == 1.5 and a["fields"][0] == [0.45, 0.82]
    # dvs-standalone: the spread is the replay's own, not the capture's.
    q = {"models": 200, "invalid": 0, "n_fields": 3, "forced": 1, "stuck": 0,
         "min_distinct": 0.5, "min_entropy": 0.9}
    r = _rec("d", config="dvs-standalone", rand={"source": "capture", "min_entropy": 0.1,
                                                  "replay": q})
    assert ci.bench_row(r)["spread"] == q


def test_sim_identity_verilator():
    b = {"simulator": "verilator", "version_text": "Verilator 5.054 2026-11-01 rev v5.054",
         "release": {"version": "5.054"}}
    assert ci.sim_identity(b) == "verilator@5.054"
    del b["release"]
    assert ci.sim_identity(b) == "verilator@5.054"


def test_manifest_hash_order_free():
    a, b = _rec("x"), _rec("y")
    assert ci.manifest_hash([a, b]) == ci.manifest_hash([b, a, a])
    assert ci.manifest_hash([a]) != ci.manifest_hash([a, b])


def test_kinds_and_artifact():
    assert all(v in ci.VARIANTS for vs in ci.KINDS.values() for v in vs)
    assert ci.KINDS["reference"][0] == "vlt-z3-default"
    assert ci.artifact_name("nightly", "20261104T053000Z", "abcdef0123") == \
        "simbench-nightly-20261104T053000Z-abcdef0"
