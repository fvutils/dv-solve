"""Mutation score: plant small soundness bugs, see which the checks catch.

    python -m tests.formal.soundness.mutation --work DIR [--only NAME] [--n 400]

Each mutant is a one-place edit of the kind the backlog is made of (a dropped
literal, an off-by-one, a missed alias resolve). The source tree is copied to
DIR, the edit applied, and two builds made (normal and -DDVS_STEP_CHECK=ON);
then the detectors run:

  harness       tests/c/test_prop_exhaustive (every propagator and explainer)
  pinned        the repros of past bugs, through dv-solve-smt2
  pytest        the pinned regression test files, against the mutant
  campaign      a fixed-seed campaign through every front door
  unknown-rate  more `unknown` answers than the unmutated baseline: a wrong
                model that validation and escalation mask
  stress        the clause-learning stress problems on the step-checker build
  checker       the pinned repros and a campaign on the step-checker build
                (invalid clauses, explanations, fixed points)

A mutant no detector catches points at a missing stimulus or oracle. The
share caught is the closure metric of docs/soundness_coverage_plan.md §5.3.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]

# name -> (file, original text, mutated text)
MUTANTS = {
    "lcg-no-own-bound": ("src/c/dvs_lcg.c",
        "if (!_explains_without_own_bound(p)) {\n            Literal own",
        "if (0) {\n            Literal own"),
    "lcg-no-guard-literal": ("src/c/dvs_lcg.c",
        "if (gid != EXPR_NULL) ADD_EXPL_LIT(_mk_lit(gid, 1, 1));\n        }\n        /* Most narrowings",
        "(void)gid;\n        }\n        /* Most narrowings"),
    "lcg-no-rewind": ("src/c/dvs_lcg.c",
        "if (_rewind_before(lcg, ctx, &applied, e) != 0)",
        "if (0 && _rewind_before(lcg, ctx, &applied, e) != 0)"),
    "lcg-no-forced-resolution": ("src/c/dvs_lcg.c",
        "force_slot = (int64_t)SEEN_IX(conflict_var, ub_crossed ? 0 : 1);",
        "(void)ub_crossed;"),
    "lcg-stale-conflict-prop": ("src/c/dvs_propagate.c",
        "    ctx->conflict_prop_ref = EXPR_NULL;\n    /* A clause conflict",
        "    /* A clause conflict"),
    "sum-explain-forward-rule": ("src/c/dvs_explain.c",
        "            out->lits[out->n_lits++] = is_lb\n                ? _mk_ub(vid, var_hi64(ctx, &ctx->vars[vid]))\n                : _mk_lb(vid, var_lo64(ctx, &ctx->vars[vid]));",
        "            out->lits[out->n_lits++] = !is_lb\n                ? _mk_ub(vid, var_hi64(ctx, &ctx->vars[vid]))\n                : _mk_lb(vid, var_lo64(ctx, &ctx->vars[vid]));"),
    "disj-explain-drops-last": ("src/c/dvs_explain.c",
        "    for (uint32_t i = 0; i < nw; i++) {\n        uint32_t vid = vids[i];\n        if (vid == var_id) continue;\n        if (out->n_lits + 2 > MAX_EXPLAIN_LITS) return -1;",
        "    for (uint32_t i = 0; i + 1 < nw; i++) {\n        uint32_t vid = vids[i];\n        if (vid == var_id) continue;\n        if (out->n_lits + 2 > MAX_EXPLAIN_LITS) return -1;"),
    "alldiff-explain-drops-last": ("src/c/dvs_explain.c",
        "Refuse rather than truncate (see explain_disj_clause). */\n    for (uint32_t i = 0; i < nw; i++) {",
        "Refuse rather than truncate (see explain_disj_clause). */\n    for (uint32_t i = 0; i + 1 < nw; i++) {"),
    "reif-edge-no-conflict": ("src/c/dvs_prop_templates.c",
        "if (y->lo >= var_repr_max(x) || x->hi <= var_repr_min(y)) return PROP_CONFLICT;",
        "if (y->lo >= var_repr_max(x) || x->hi <= var_repr_min(y)) return PROP_OK;"),
    "reif-guard0-off-by-one": ("src/c/dvs_prop_templates.c",
        "if ((r = ctx_tighten_lb32(ctx, xid, y->lo + 1)) != PROP_OK) return r;\n        if ((r = ctx_tighten_ub32(ctx, yid, x->hi - 1)) != PROP_OK) return r;",
        "if ((r = ctx_tighten_lb32(ctx, xid, y->lo + 2)) != PROP_OK) return r;\n        if ((r = ctx_tighten_ub32(ctx, yid, x->hi - 1)) != PROP_OK) return r;"),
    "ite-entail-half-bounds": ("src/c/dvs_prop_templates.c",
        "if (rlo == rhi && alo == ahi && alo == rlo) return PROP_ENTAILED;",
        "if (rlo == rhi && alo == rlo) return PROP_ENTAILED;"),
    "mul32-ignores-zero": ("src/c/dvs_prop_templates.c",
        "if ((a->lo == 0 && a->hi == 0) || (b->lo == 0 && b->hi == 0)) {",
        "if (0) {"),
    "lt64-unsigned-as-signed": ("src/c/dvs_prop_templates.c",
        "        int xs = (xv->flags & VAR_SIGNED) || xv->width < 64;\n        int ys = (yv->flags & VAR_SIGNED) || yv->width < 64;",
        "        int xs = 1;\n        int ys = 1;"),
    "gated-no-alias-resolve": ("src/c/dvs_compile.c",
        "vid = _resolve(ctx, vid);   /* x == y merges: act on the kept var */",
        "/* no resolve */"),
    "gated-fallback-ungated": ("src/c/dvs_compile.c",
        "    uint32_t g = _bool_to_var(ctx, sp, root);\n    if (g == EXPR_NULL) return 0;\n    prop_add_implication_32(ctx, guard_id, g, 1, /*is_ub=*/0, 0);\n    return 1;",
        "    (void)guard_id;\n    return _compile_constraint(ctx, sp, root);"),
    "udiv-zero-unguarded": ("src/c/smt2/smt2_frontend.c",
        "    if (bp && *(const ExprKind *)bp == EXPR_CONST &&\n        ((const ExprConst *)bp)->value != 0)\n        return q;",
        "    (void)bp;\n        return q;"),
    "frontend-compiled-flag": ("src/c/smt2/smt2_frontend.c",
        "        if (rc >= 0) {\n            /* rc > 0 (some constraints left uncompiled;",
        "        if (rc == 0) {\n            /* rc > 0 (some constraints left uncompiled;"),
    "bvadd-wrap-width": ("src/c/dvs_prop_templates.c",
        "return _prop_add_bvbin_64(ctx, r_id, a_id, b_id, width, priority, _fire_bvadd_64);",
        "return _prop_add_bvbin_64(ctx, r_id, a_id, b_id, width > 1 ? width - 1 : width, priority, _fire_bvadd_64);"),
}

# Repros of past bugs: (SMT-LIB2 script, interactive?, expected answers).
_H = "(set-logic QF_BV)(declare-const x (_ BitVec 4))(declare-const y (_ BitVec 4))(declare-const z (_ BitVec 4))"
PINNED = [
    (_H + "(assert (distinct (bvsub z x) z))(assert (bvugt (bvsub z x) (_ bv14 4)))(check-sat)", False, ["sat"]),
    (_H + "(assert (bvule (bvand y z) x))(assert (bvugt (bvadd x y) x))(assert (bvugt y z))"
          "(assert (distinct (bvand y z) (_ bv0 4)))(check-sat)", False, ["sat"]),
    (_H + "(assert (= (bvsub z x) (_ bv12 4)))(assert (bvult (bvadd x y) y))(assert (bvult x z))(check-sat)",
     False, ["sat"]),
    (_H + "(assert (= (bvand y z) (_ bv3 4)))(assert (bvule (bvand y z) x))(check-sat)", False, ["sat"]),
    (_H + "(assert (ite (= x y) false true))(check-sat)", False, ["sat"]),
    (_H + "(assert (ite (= z (_ bv5 4)) (bvuge y x) (bvult y x)))(check-sat)", False, ["sat"]),
    (_H + "(assert (bvuge z (bvudiv z z)))(assert (= z (_ bv0 4)))(check-sat)", False, ["unsat"]),
    (_H + "(assert (xor true (= x (_ bv0 4))))(check-sat)(assert false)(check-sat)", True, ["sat", "unsat"]),
]


def _reset(src_root: Path) -> None:
    """Restore every file a mutant edits from the repository."""
    for path in {m[0] for m in MUTANTS.values()}:
        dst = src_root / path
        if dst.read_bytes() != (REPO / path).read_bytes():
            # A fresh mtime, so make rebuilds it: copy2 kept the repository's
            # older timestamp and make kept the previous mutant's objects.
            shutil.copyfile(REPO / path, dst)
            os.utime(dst)


def _run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def _apply(src_root: Path, name: str) -> bool:
    path, old, new = MUTANTS[name]
    f = src_root / path
    s = f.read_text()
    if old not in s:
        return False
    f.write_text(s.replace(old, new, 1))
    os.utime(f)
    return True


PYTESTS = ["tests/formal/test_lcg_soundness.py", "tests/formal/test_bool_ite_constraint.py",
           "tests/formal/test_incremental_protocol.py", "tests/formal/soundness/test_regressions.py",
           "tests/unit/test_gated_constraints.py", "tests/unit/test_lcg_builder.py",
           "tests/unit/test_wide_watch.py", "tests/unit/test_lcg_array_select.py"]


def _pinned(exe: Path) -> bool:
    for script, inter, exp in PINNED:
        try:
            r = _run([str(exe)] + (["--interactive"] if inter else []), input=script, timeout=60)
            got = [w for w in r.stdout.split() if w in ("sat", "unsat", "unknown")]
        except subprocess.TimeoutExpired:
            got = ["timeout"]
        if got != exp or "[step-check] INVALID" in (r.stderr if got != ["timeout"] else ""):
            return True
    return False


def _campaign(build: Path, n: int, seed: int, extra_env=None, limit_ms: int = 10000):
    env = dict(os.environ, DVS_SOLVER_PATH=str(build), PYTHONPATH=str(REPO / "src"),
               **(extra_env or {}))
    r = _run([sys.executable, "-m", "tests.formal.soundness.campaign", "--seed", str(seed),
              "--n", str(n), "--exe", str(build / "dv-solve-smt2"), "--out",
              str(build / "mut_regressions"), "--builder-limit-ms", str(limit_ms),
              "--wide", "0.1"],
             cwd=str(REPO), env=env, timeout=7200)
    stats = {}
    for line in r.stdout.splitlines():
        if line.startswith("STATS "):
            import ast
            stats = ast.literal_eval(line[6:])
    invalid = "[step-check] INVALID" in r.stdout + r.stderr
    return r.returncode != 0 or invalid, stats


def _unknowns(stats: dict) -> int:
    return sum(v for k, v in stats.items() if k.endswith(":unknown"))


def detect(src: Path, n: int, seed: int, baseline_unknown: int) -> dict:
    build, check = src / "build", src / "build-check"
    found = {}
    found["harness"] = _run([str(build / "test_prop_exhaustive")], timeout=900).returncode != 0
    found["pinned"] = _pinned(build / "dv-solve-smt2")
    env = dict(os.environ, PYTHONPATH=str(src / "src"))
    r = _run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider", *PYTESTS],
             cwd=str(src), env=env, timeout=3600)
    found["pytest"] = r.returncode not in (0, 5)
    bad, stats = _campaign(build, n, seed)
    found["campaign"] = bad
    # Bugs that validation and escalation mask still show as more `unknown`.
    found["unknown-rate"] = _unknowns(stats) > baseline_unknown
    stress = sorted((REPO / "tests" / "formal" / "soundness" / "lcg_stress").glob("*.smt2"))
    hit = False
    for f in stress:
        try:
            r = _run([str(check / "dv-solve-smt2"), str(f)], timeout=300,
                     env=dict(os.environ, DV_STEP_CHECK_CONTINUE="1"))
            hit = hit or "[step-check] INVALID" in r.stderr
        except subprocess.TimeoutExpired:
            hit = True
    found["stress"] = hit
    cenv = {"DV_STEP_CHECK_CONTINUE": "1"}
    found["checker"] = _pinned(check / "dv-solve-smt2") or \
        _campaign(check, n, seed + 1, cenv, limit_ms=120000)[0]
    return found


def _build(src: Path) -> bool:
    ok = True
    for b in ("build", "build-check"):
        r = _run(["cmake", "--build", str(src / b), "-j16", "--target", "dv_solve",
                  "dv-solve-smt2", "test_prop_exhaustive"])
        ok = ok and r.returncode == 0
    return ok


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--n", type=int, default=400)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args(argv)
    work = Path(a.work)
    src = work / "src_tree"
    if not (src / "build-check" / "CMakeCache.txt").exists():
        shutil.rmtree(src, ignore_errors=True)
        shutil.copytree(REPO, src, symlinks=True, ignore=shutil.ignore_patterns(
            "build", "build-*", "build_*", ".git", ".claude", "packages", "out",
            "__pycache__", "results"))
        (src / "packages").symlink_to(REPO / "packages")
        _run(["cmake", "-S", str(src), "-B", str(src / "build"), "-DCMAKE_BUILD_TYPE=Release"],
             check=True)
        _run(["cmake", "-S", str(src), "-B", str(src / "build-check"),
              "-DCMAKE_BUILD_TYPE=Release", "-DDVS_STEP_CHECK=ON"], check=True)
    _reset(src)
    _build(src)
    env = dict(os.environ, PYTHONPATH=str(src / "src"))
    r = _run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider", *PYTESTS],
             cwd=str(src), env=env, timeout=3600)
    if r.returncode != 0:
        print("baseline pytest fails; fix the copy before trusting the pytest detector:\n"
              + r.stdout[-3000:], flush=True)
        return 2
    _, base = _campaign(src / "build", a.n, a.seed)
    baseline_unknown = _unknowns(base)
    print(f"baseline: {baseline_unknown} unknown answers in the campaign", flush=True)
    names = [m for m in MUTANTS if not a.only or a.only in m]
    caught = applied = 0
    for name in names:
        _reset(src)
        if not _apply(src, name):
            print(f"SKIP {name}: edit does not apply (code changed?)", flush=True)
            continue
        if not _build(src):
            print(f"SKIP {name}: does not build", flush=True)
            continue
        applied += 1
        found = detect(src, a.n, a.seed, baseline_unknown)
        hit = any(found.values())
        caught += hit
        print(f"{'CAUGHT ' if hit else 'MISSED '} {name:28s} "
              + " ".join(f"{k}={'y' if v else '-'}" for k, v in found.items()), flush=True)
    _reset(src)
    _build(src)
    print(f"mutation score: {caught}/{applied}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
