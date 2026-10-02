"""Verilator 5.046's randomize() protocol, driving any SMT-LIB2 solver over a pipe.

A re-implementation of `VlRandomizer::next()` in
`share/verilator/include/verilated_random.cpp` of the pinned Verilator
(packages/verilator-bin, tools.lock.json), for problems without randc,
`unique` arrays or solve-before. One solver process lives for the whole run,
as in Verilator (`getSolver()` opens it once). Per randomize():

    (set-option :produce-models true) (set-logic QF_ABV)
    (define-fun __Vbv ...) (define-fun __Vbool ...)
    (declare-fun <var> () (_ BitVec <w>))      every rand variable, by name
    (assert (= #b1 <constraint>))              every constraint
    (check-sat)  -> sat: (get-value (<vars>))
    up to 4 times, while sat:
        (assert (= #b<r> <1-bit XOR of about half of all rand bits>))
        (check-sat)  -> sat: (get-value ...)  unsat: stop
    (reset)

The result is the last satisfying model. VlRNG is replaced by a seeded
`random.Random`, so the hash constraints have the same shape and the same
distribution, not the same bits. The fidelity test
(tests/unit/test_vlt_protocol.py) compares this stream with one captured
from the real Verilator.
"""
from __future__ import annotations

import os
import random
import re
import resource
import subprocess
import threading
import time

PROTOCOL = "verilator-5.046"
CALL_BUDGET_S = 10.0  # one randomize(); past it the solver is killed
HASH_LEN = 1          # _VL_SOLVER_HASH_LEN
HASH_LEN_TOTAL = 4    # _VL_SOLVER_HASH_LEN_TOTAL

# v0.1.0 keeps a 10 MB frontend struct on the stack; every arm gets the same
# limit (tests/perf/run_sat.py).
STACK_BYTES = 64 << 20


def _limits():
    resource.setrlimit(resource.RLIMIT_STACK, (STACK_BYTES, STACK_BYTES))


_PRELUDE = ("(set-option :produce-models true)\n"
            "(set-logic QF_ABV)\n"
            "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n"
            "(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))\n")
_VAL = re.compile(r"\(\s*([A-Za-z_][\w.$]*)\s+(#b[01]+|#x[0-9a-fA-F]+|\(_\s+bv(\d+)\s+\d+\))\s*\)")


class ProtocolError(RuntimeError):
    """The solver answered something Verilator would warn about."""


def _value(lit: str, dec: str) -> int:
    if dec:
        return int(dec)
    return int(lit[2:], 2) if lit.startswith("#b") else int(lit[2:], 16)


def hash_constraint(widths: dict, rng: random.Random, bits: int = HASH_LEN) -> str:
    """`VlRandomizer::randomConstraint`: `(= #b<r> <XOR of ~half the bits>)`."""
    names = sorted(widths)            # m_vars is a std::map: ordered by name
    var_bits = sum(widths[n] for n in names)
    h = rng.getrandbits(32) & ((1 << bits) - 1)
    out = "(= #b" + "".join("1" if h >> i & 1 else "0" for i in range(bits - 1, -1, -1))
    if bits > 1:
        out += " (concat"
    for _ in range(bits):
        left, want = var_bits, (var_bits + 1) // 2
        if var_bits > 2:
            out += " (bvxor"
        for n in names:
            for j in range(widths[n]):
                emit = rng.getrandbits(32) % left < want
                left -= 1
                if emit:
                    out += f" ((_ extract {j} {j}) {n})"
                    want -= 1
                    if want == 0:
                        break
            if want == 0:
                break
        if var_bits > 2:
            out += ")"
    if bits > 1:
        out += ")"
    return out + ")"


def randomize_script(widths: dict, cons: list) -> str:
    """The fixed part of one randomize(): prelude, declarations, constraints."""
    s = _PRELUDE
    s += "".join(f"(declare-fun {n} () (_ BitVec {widths[n]}))\n" for n in sorted(widths))
    s += "".join(f"(assert (= #b1 {c}))\n" for c in cons)
    return s


class Session:
    """One solver process, randomized against repeatedly.

    `cons` are Verilator-form constraints: 1-bit bit-vector expressions, as
    Verilator emits them (`(__Vbv (bvult a b))`). `log`, when given, receives
    every command sent, for the fidelity test.
    """

    def __init__(self, argv: list, widths: dict, cons: list, env: dict | None = None,
                 log: list | None = None):
        self.widths = dict(widths)
        self.names = sorted(widths)
        self.fixed = randomize_script(widths, cons)
        # emitGetValue writes " <name>" per variable: "(get-value ( a b))".
        self.getv = "(get-value (" + "".join(" " + n for n in self.names) + "))\n"
        self.log = log
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                  preexec_fn=_limits,
                                  env={**os.environ, **(env or {})})
        # getSolver()'s liveness probe, sent once per process.
        self._send("(set-logic QF_ABV)\n(check-sat)\n(reset)\n")
        if self._line() != "sat":
            raise ProtocolError("solver failed the liveness probe")

    def _send(self, s: str) -> None:
        if self.log is not None:
            self.log.append(s)
        self.p.stdin.write(s)
        self.p.stdin.flush()

    def _line(self) -> str:
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise ProtocolError("solver exited")
            if line.strip():
                return line.strip()

    def _model(self) -> dict:
        self._send(self.getv)
        buf, depth = "", 0
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise ProtocolError("solver exited during get-value")
            buf += line
            depth += line.count("(") - line.count(")")
            if depth <= 0 and "(" in buf:
                break
        m = {n: _value(lit, dec) for n, lit, dec in _VAL.findall(buf)}
        if set(m) != set(self.names):
            raise ProtocolError("unparsable get-value response")
        return m

    def randomize(self, rng: random.Random) -> tuple:
        """One randomize(): (model, number of check-sats). A call that runs
        past CALL_BUDGET_S kills the solver and raises ProtocolError."""
        watchdog = threading.Timer(CALL_BUDGET_S, self.p.kill)
        watchdog.start()
        try:
            return self._randomize(rng)
        except ProtocolError:
            if not watchdog.is_alive():
                raise ProtocolError(f"randomize() took over {CALL_BUDGET_S:g} s")
            raise
        finally:
            watchdog.cancel()

    def _randomize(self, rng: random.Random) -> tuple:
        self._send(self.fixed + "(check-sat)\n")
        v = self._line()
        if v != "sat":
            raise ProtocolError(f"base problem: {v}")
        model, checks = self._model(), 1
        for _ in range(HASH_LEN_TOTAL):
            self._send("(assert " + hash_constraint(self.widths, rng) + ")\n\n(check-sat)\n")
            checks += 1
            v = self._line()
            if v == "unsat":
                break
            if v != "sat":
                raise ProtocolError(f"hash check: {v}")
            model = self._model()
        self._send("(reset)\n")
        return model, checks

    def close(self) -> float:
        """End the process; its CPU time in seconds (user + system)."""
        try:
            self.p.stdin.write("(exit)\n")
            self.p.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        _, status, ru = os.wait4(self.p.pid, 0)
        self.p.returncode = 0
        self.p.stdout.close()
        return ru.ru_utime + ru.ru_stime


def sample(argv: list, widths: dict, cons: list, n: int, seed: int,
           env: dict | None = None) -> dict:
    """n randomize() calls in one process: models, per-call wall time, CPU."""
    rng = random.Random(seed)
    s = Session(argv, widths, cons, env)
    models, wall, checks = [], [], 0
    try:
        for _ in range(n):
            t0 = time.perf_counter()
            m, c = s.randomize(rng)
            wall.append(time.perf_counter() - t0)
            models.append(tuple(m[k] for k in sorted(widths)))
            checks += c
    finally:
        cpu = s.close()
    return {"models": models, "wall_s": wall, "cpu_s": cpu, "checks": checks}
