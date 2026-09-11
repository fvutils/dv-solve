"""Generative QF_ABV fuzzer for the DV_ARRAY word-level array engine.

Emits random store chains, selects at shared + distinct indices, array
equalities (between vars and store terms, both polarities), and random
conjunction assertions. Deterministic per seed. Companion to fuzz_bv_smt2.py;
consumed by test_array_lazy.py, which cross-checks the lazy engine against z3.

The op mix deliberately stresses the parts that array reasoning gets wrong:
- read-over-write hits and misses down deep store chains,
- congruence (same array, model-equal indices),
- array equality between two *store terms* (extensionality + store-index
  instantiation), and structural sharing (the same term written twice).
"""
from __future__ import annotations

import random


def generate_problem(seed: int) -> str:
    r = random.Random(seed)
    aw = r.choice([2, 3, 4])
    dw = r.choice([4, 8])
    lines = ["(set-logic QF_ABV)"]
    narr = r.randint(1, 3)
    bases = [f"A{i}" for i in range(narr)]
    for a in bases:
        lines.append(f"(declare-fun {a} () (Array (_ BitVec {aw}) (_ BitVec {dw})))")
    idxs = [f"i{i}" for i in range(r.randint(2, 5))]
    for i in idxs:
        lines.append(f"(declare-fun {i} () (_ BitVec {aw}))")
    vals = [f"v{i}" for i in range(r.randint(2, 5))]
    for v in vals:
        lines.append(f"(declare-fun {v} () (_ BitVec {dw}))")

    # A pool of array terms: the bases plus random store chains over them.
    terms = list(bases)
    for _ in range(r.randint(1, 6)):
        t = r.choice(terms)
        terms.append(f"(store {t} {r.choice(idxs)} {r.choice(vals)})")

    def sel() -> str:
        return f"(select {r.choice(terms)} {r.choice(idxs)})"

    preds = []
    for _ in range(r.randint(1, 5)):
        k = r.random()
        if k < 0.5:
            p = f"(= {sel()} {sel()})"
        elif k < 0.75:
            p = f"(= {sel()} {r.choice(vals)})"
        else:  # array equality (var/var, var/store, store/store)
            p = f"(= {r.choice(terms)} {r.choice(terms)})"
        if r.random() < 0.4:
            p = f"(not {p})"
        preds.append(p)

    if len(preds) == 1:
        lines.append(f"(assert {preds[0]})")
    else:
        lines.append("(assert (and " + " ".join(preds) + "))")
    lines.append("(check-sat)")
    return "\n".join(lines) + "\n"
