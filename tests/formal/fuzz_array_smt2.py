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


def generate_session(seed: int) -> str:
    """An incremental session: several check-sats with asserts, push and pop
    between them, over arrays of any address width (large ones take the
    word-level engine by default, promoted from the sparse path on their first
    symbolic index, store, `=` or `ite`). Constant indices are mixed in so
    promotion meets element vars the sparse path already made."""
    r = random.Random(seed)
    aw = r.choice([3, 12, 16, 32])
    dw = r.choice([4, 8])
    lines = ["(set-logic QF_ABV)"]
    bases = [f"A{i}" for i in range(r.randint(1, 3))]
    for a in bases:
        lines.append(f"(declare-fun {a} () (Array (_ BitVec {aw}) (_ BitVec {dw})))")
    idxs = [f"i{i}" for i in range(r.randint(2, 4))]
    for i in idxs:
        lines.append(f"(declare-fun {i} () (_ BitVec {aw}))")
    vals = [f"v{i}" for i in range(r.randint(2, 4))]
    for v in vals:
        lines.append(f"(declare-fun {v} () (_ BitVec {dw}))")
    consts = [f"(_ bv{r.randrange(min(1 << aw, 8))} {aw})" for _ in range(2)]

    def idx() -> str:
        return r.choice(consts) if r.random() < 0.35 else r.choice(idxs)

    def term(arrs) -> str:
        t = r.choice(arrs)
        for _ in range(r.choice([0, 0, 1, 2])):
            t = f"(store {t} {idx()} {r.choice(vals)})"
        return t

    def pred(arrs) -> str:
        k = r.random()
        if k < 0.45:
            p = f"(= (select {term(arrs)} {idx()}) (select {term(arrs)} {idx()}))"
        elif k < 0.75:
            p = f"(= (select {term(arrs)} {idx()}) {r.choice(vals)})"
        elif k < 0.9:
            p = f"(= {r.choice(idxs)} {r.choice(idxs + consts)})"
        else:
            p = f"(= {term(arrs)} {term(arrs)})"
        return f"(not {p})" if r.random() < 0.35 else p

    # Constant-index reads first, so a later symbolic one promotes a sparse array
    # that already has element vars.
    lines.append(f"(assert (= (select {r.choice(bases)} {consts[0]}) {r.choice(vals)}))")
    depth, n_scoped = 0, 0
    for _ in range(r.randint(2, 5)):
        arrs = list(bases)
        if r.random() < 0.4:
            lines.append("(push 1)")
            depth += 1
            if r.random() < 0.4:
                s = f"S{n_scoped}"
                n_scoped += 1
                lines.append(f"(declare-fun {s} () (Array (_ BitVec {aw}) (_ BitVec {dw})))")
                arrs.append(s)
        for _ in range(r.randint(1, 2)):
            lines.append(f"(assert {pred(arrs)})")
        lines.append("(check-sat)")
        if depth and r.random() < 0.6:
            lines.append("(pop 1)")
            depth -= 1
            lines.append("(check-sat)")
    return "\n".join(lines) + "\n"
