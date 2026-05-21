#!/usr/bin/env python3
"""Generate Tier 2 BMC and k-induction SMT-LIB2 files from Zuspec RTL models.

Usage:
    direnv exec . python tests/formal/tier2/generate_tier2_smt2.py
"""
from __future__ import annotations

import importlib
import sys
from pathlib import Path

# Ensure packages are importable
REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "packages" / "zuspec-dataclasses" / "src"))
sys.path.insert(0, str(REPO_ROOT / "packages" / "zuspec-be-fv" / "src"))
sys.path.insert(0, str(REPO_ROOT))

from zuspec.dataclasses.data_model_factory import DataModelFactory
from zuspec.be.fv.rtl import RTLToSMT2Translator
from zuspec.be.fv.rtl.smt2_module import SMT2Module
from zuspec.be.fv.verification.bmc_smt2 import generate_bmc_smt2
from zuspec.be.fv.verification.k_induction_smt2 import generate_k_induction_step_smt2

from tests.formal.tier2.models.benchmark_config import TIER2_BENCHMARKS

MODELS_DIR = Path(__file__).resolve().parent / "models"
SMT2_DIR = REPO_ROOT / "tests" / "formal" / "smt2" / "tier2"


def load_class(module_name: str, class_name: str):
    """Dynamically import a model class."""
    mod = importlib.import_module(f"tests.formal.tier2.models.{module_name}")
    return getattr(mod, class_name)


def generate_bmc_with_reset(
    translator: RTLToSMT2Translator,
    module: SMT2Module,
    *,
    depth: int,
) -> str:
    """Generate BMC SMT2 with explicit reset=true at state_0.

    The standard generate_bmc_smt2 relies on the _i predicate for initial
    state constraints.  When the translator emits _i = true (common for
    components using 'if self.reset' instead of field-level reset= values),
    the BMC problem is under-constrained.  This wrapper fixes the issue by
    asserting reset=true at state_0 and reset=false for subsequent states.
    """
    # check_all_steps=False would only check the final step, but we want
    # to check all steps EXCEPT state_0 (which is the reset state).
    # Generate the base with check_all_steps=True, then replace the
    # assertion disjunction to skip state_0.
    base = generate_bmc_smt2(translator, module, depth=depth, get_model=False,
                              check_all_steps=False)
    # The base asserts (not (_a state_N)) for the final state only.
    # Replace with a disjunction over states 1..depth (skip state_0).
    old_assert = f'(assert (not (|{module.name}_a| state_{depth})))'
    if depth > 0:
        disj_parts = [f'(not (|{module.name}_a| state_{k}))' for k in range(1, depth + 1)]
        if len(disj_parts) == 1:
            new_assert = f'(assert {disj_parts[0]})'
        else:
            new_assert = f'(assert (or {" ".join(disj_parts)}))'
        base = base.replace(old_assert, new_assert)

    # Find the reset signal's SMT2 name (may be in inputs, wires, or outputs)
    reset_smt_name = None
    all_signals = module.get_all_signals() if hasattr(module, 'get_all_signals') else {}
    if not all_signals:
        all_signals = {**module.inputs, **module.outputs, **module.wires}
    for sig in all_signals.values():
        if sig.name == 'reset':
            reset_smt_name = sig.smt_name
            break

    if reset_smt_name is None:
        return base

    # Build reset constraint lines
    reset_lines = [
        f"; Force reset=true at state_0, reset=false thereafter",
        f"(assert (|{module.name}#{reset_smt_name}| state_0))",
    ]
    for k in range(1, depth + 1):
        reset_lines.append(
            f"(assert (not (|{module.name}#{reset_smt_name}| state_{k})))"
        )

    # Insert before (check-sat)
    lines = base.split("\n")
    idx = next(i for i, l in enumerate(lines) if l.strip() == "(check-sat)")
    for j, rl in enumerate(reset_lines):
        lines.insert(idx + j, rl)

    return "\n".join(lines)


def generate_kind_with_reset(
    translator: RTLToSMT2Translator,
    module: SMT2Module,
    *,
    k: int,
) -> str:
    """Generate k-induction SMT2 with reset=false for all states.

    In k-induction the inductive step, we assume the property holds for
    states 0..k and check if it can fail at k+1.  All states should be
    non-reset states.
    """
    base = generate_k_induction_step_smt2(translator, module, k=k, get_model=False)

    reset_smt_name = None
    all_signals = module.get_all_signals() if hasattr(module, 'get_all_signals') else {}
    if not all_signals:
        all_signals = {**module.inputs, **module.outputs, **module.wires}
    for sig in all_signals.values():
        if sig.name == 'reset':
            reset_smt_name = sig.smt_name
            break

    if reset_smt_name is None:
        return base

    reset_lines = [f"; Force reset=false for all k-induction states"]
    for i in range(k + 2):
        reset_lines.append(
            f"(assert (not (|{module.name}#{reset_smt_name}| state_{i})))"
        )

    lines = base.split("\n")
    idx = next(i for i, l in enumerate(lines) if l.strip() == "(check-sat)")
    for j, rl in enumerate(reset_lines):
        lines.insert(idx + j, rl)

    return "\n".join(lines)


def main():
    SMT2_DIR.mkdir(parents=True, exist_ok=True)

    total = 0
    errors = 0

    for bench_name, cfg in TIER2_BENCHMARKS.items():
        print(f"\n=== {bench_name} ===")

        # Load and translate the model
        try:
            cls = load_class(cfg["module"], cfg["class_name"])
        except Exception as e:
            print(f"  SKIP (import failed): {e}")
            errors += 1
            continue

        try:
            factory = DataModelFactory()
            ctx = factory.build(cls)
            comp = ctx.type_m[cls.__qualname__]

            tr = RTLToSMT2Translator()
            module = tr.translate_component(comp)
        except Exception as e:
            print(f"  SKIP (translation failed): {e}")
            errors += 1
            continue

        print(f"  Transitions: {len(module.transitions)}")
        print(f"  Assertions:  {len(module.assertions)}")
        print(f"  Coverage:    {len(module.coverage_goals)}")
        print(f"  Registers:   {list(module.registers.keys())}")

        # Generate BMC files with reset
        for depth in cfg["bmc_depths"]:
            try:
                smt2 = generate_bmc_with_reset(tr, module, depth=depth)
                out_path = SMT2_DIR / f"{bench_name}_bmc_d{depth}.smt2"
                out_path.write_text(smt2)
                print(f"  BMC d={depth:3d} -> {out_path.name}")
                total += 1
            except Exception as e:
                print(f"  BMC d={depth}: ERROR: {e}")
                errors += 1

        # Generate k-induction files with reset=false
        for k in cfg["k_induction_ks"]:
            try:
                smt2 = generate_kind_with_reset(tr, module, k=k)
                out_path = SMT2_DIR / f"{bench_name}_kind_k{k}.smt2"
                out_path.write_text(smt2)
                print(f"  k-ind k={k:3d} -> {out_path.name}")
                total += 1
            except Exception as e:
                print(f"  k-ind k={k}: ERROR: {e}")
                errors += 1

    print(f"\n{'='*50}")
    print(f"Generated {total} SMT2 files ({errors} errors)")
    print(f"Output directory: {SMT2_DIR}")

    return 0 if errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
