"""A context reused by checkpoint/pin/solve/restore does not grow.

A caller that solves one problem many times compiles it once and, per solve,
takes a checkpoint, pins values, solves and restores. Every trail entry made
after the checkpoint is dead once it is restored, so the blocks holding them
must go back to the block allocator. They did not: a solve seals its
level-0 baseline with a fresh push of the dynamic stack, and the restore
popped to that push rather than to the checkpoint's, keeping every entry the
pins and the solve's first propagation made. A context then took a new block
every few solves for as long as it was reused.

The backing allocator here counts what the context's block allocator asks
for, so the test needs no process-memory measurement.
"""
import ctypes

from dv_solve import ctx as ctx_mod
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx
from dv_solve.problem import BIN_LT

SOLVE_OK = 0
N_VARS = 24
BLOCK = 1 << 16


class _Alloc(ctypes.Structure):
    pass


_ALLOC_FN = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.POINTER(_Alloc), ctypes.c_size_t)
_RELEASE_FN = ctypes.CFUNCTYPE(None, ctypes.POINTER(_Alloc), ctypes.c_void_p, ctypes.c_size_t)
_Alloc._fields_ = [("alloc", _ALLOC_FN), ("release", _RELEASE_FN)]

_libc = ctypes.CDLL(None)
_libc.malloc.restype = ctypes.c_void_p
_libc.malloc.argtypes = [ctypes.c_size_t]
_libc.free.restype = None
_libc.free.argtypes = [ctypes.c_void_p]


class _Counting:
    """A dvs_alloc_t backed by malloc that counts the blocks it hands out."""

    def __init__(self):
        self.blocks = 0
        self._fns = (_ALLOC_FN(self._alloc), _RELEASE_FN(self._release))
        self.alloc = _Alloc(*self._fns)

    def _alloc(self, _self, size):
        if size == BLOCK:
            self.blocks += 1
        return _libc.malloc(size)

    def _release(self, _self, ptr, size):
        if size == BLOCK:
            self.blocks -= 1
        _libc.free(ptr)


def _ctx(counting):
    """v0 < v1 < ... < v23, each 8 bits, compiled on *counting*'s blocks."""
    b = SolveProblemBuilder()
    for v in range(N_VARS):
        b.add_var(v, 8, False, 0, 255)
    for v in range(N_VARS - 1):
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(v), b.expr_var(v + 1)))
    buf, _ = b.finalize()
    lib = ctx_mod._load_lib()
    create = lib.dvs_block_alloc_create
    try:
        lib.dvs_block_alloc_create = lambda _a, size: create(
            ctypes.cast(ctypes.byref(counting.alloc), ctypes.c_void_p), size)
        ctx = SolveCtx(buf, ctx_buf_size=BLOCK)
    finally:
        lib.dvs_block_alloc_create = create
    return b, ctx


def test_reuse_takes_no_new_blocks():
    counting = _Counting()
    b, ctx = _ctx(counting)
    try:
        def cycle(seed):
            cp = ctx.checkpoint()
            for v in range(0, N_VARS, 3):
                assert ctx.pin(v, 10 * v + seed % 3)
            assert ctx.solve(seed=seed) == SOLVE_OK
            vals = [ctx.get_value(v) for v in range(N_VARS)]
            assert vals == sorted(set(vals))
            ctx.restore(cp)

        for seed in range(50):
            cycle(seed)
        warm = counting.blocks
        for seed in range(50, 3000):
            cycle(seed)
        assert counting.blocks == warm, (warm, counting.blocks)
    finally:
        ctx.destroy()
        b.destroy()
