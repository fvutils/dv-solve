"""V-P02: post-verilation fix for Verilator's constraint codegen.

For `x inside {queue}` / `.sum()` over a runtime-sized container, Verilator
generates C++ that assembles an n-ary SMT term at run time:

    return ret.empty() ? "<identity>" : "(<op>" + ret + ")";

With exactly one element this yields a unary (bvor X) / (bvand X) / (bvadd X),
which SMT-LIB does not allow (bv ops are :left-assoc, >= 2 args). z3 and
dv-solve tolerate it; bitwuzla rejects it and the solver session dies. The
empty-case literal is the operator's identity, so prepending it is
semantics-preserving:  "(<op> <identity>" + ret + ")".
(From solver-bench/scripts/fix_nary.py.)
"""
import re
from pathlib import Path

_PAT = re.compile(r'ret\.empty\(\) \? "([^"]+)" : "\((bvor|bvand|bvadd|bvxor|bvmul)" \+ ret \+ "\)"')


def patch_dir(d) -> int:
    """Patch every generated .cpp under d; return the number of sites."""
    total = 0
    for f in Path(d).glob("*.cpp"):
        s = f.read_text()
        out, n = _PAT.subn(lambda m: f'ret.empty() ? "{m.group(1)}" : '
                                     f'"({m.group(2)} {m.group(1)}" + ret + ")"', s)
        if n:
            f.write_text(out)
            total += n
    return total
