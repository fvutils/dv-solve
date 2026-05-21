#!/usr/bin/env python3
"""Generate SMT-LIB2 files from all Tier 1 @zdc.dataclass benchmarks.

Usage:
    direnv exec . python tests/formal/generate_tier1_smt2.py

Outputs .smt2 files to tests/formal/smt2/tier1/.
"""
from __future__ import annotations

import importlib
import inspect
import sys
import warnings
from pathlib import Path

# Ensure the bench directory is importable (for `from solvers import solvers`)
_REPO = Path(__file__).resolve().parents[2]
_BENCH_DIR = str(_REPO / "tests" / "bench")
if _BENCH_DIR not in sys.path:
    sys.path.insert(0, _BENCH_DIR)

import zuspec.dataclasses as zdc
from zuspec.be.fv.smt2.rand_emitter import RandSMT2Emitter

SMT2_DIR = Path(__file__).resolve().parent / "smt2" / "tier1"


def _discover_zdc_classes(module) -> list[tuple[str, type]]:
    """Find all @zdc.dataclass classes with rand fields in a module."""
    results = []
    for name, obj in inspect.getmembers(module, inspect.isclass):
        # Skip imports from other modules
        if obj.__module__ != module.__name__:
            continue
        try:
            fields = zdc.extract_rand_fields(obj)
            if fields:
                results.append((name, obj))
        except Exception:
            pass
    return results


def _discover_zdc_objects(module) -> list[tuple[str, type]]:
    """Find @zdc.dataclass objects (including dynamically created ones)."""
    results = _discover_zdc_classes(module)

    # Also check module-level names that are classes but might have
    # __module__ set to something else (dynamic creation)
    for name in dir(module):
        obj = getattr(module, name)
        if not isinstance(obj, type):
            continue
        # Skip if already found
        if any(o is obj for _, o in results):
            continue
        try:
            fields = zdc.extract_rand_fields(obj)
            if fields:
                results.append((name, obj))
        except Exception:
            pass
    return results


def main():
    SMT2_DIR.mkdir(parents=True, exist_ok=True)

    bench_dir = _REPO / "tests" / "bench"
    test_files = sorted(bench_dir.glob("test_*.py"))

    emitter = RandSMT2Emitter()
    generated = 0
    skipped = []

    for tf in test_files:
        module_name = tf.stem
        try:
            module = importlib.import_module(module_name)
        except Exception as exc:
            warnings.warn(f"Could not import {module_name}: {exc}")
            skipped.append((module_name, str(exc)))
            continue

        classes = _discover_zdc_objects(module)
        if not classes:
            skipped.append((module_name, "no @zdc.dataclass with rand fields"))
            continue

        for cls_name, cls in classes:
            out_name = cls_name.lower()
            out_path = SMT2_DIR / f"{out_name}.smt2"
            try:
                smt2_text = emitter.emit(cls, seed=0)
                out_path.write_text(smt2_text)
                print(f"  OK  {module_name} -> {cls_name} -> {out_path.name}")
                generated += 1
            except Exception as exc:
                warnings.warn(f"  FAIL {module_name}.{cls_name}: {exc}")
                skipped.append((f"{module_name}.{cls_name}", str(exc)))

    print(f"\nGenerated {generated} .smt2 files in {SMT2_DIR}")
    if skipped:
        print(f"Skipped {len(skipped)}:")
        for name, reason in skipped:
            print(f"  {name}: {reason}")


if __name__ == "__main__":
    main()
