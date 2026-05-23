; Regression for bvnot propagator missing bit-width mask. Before fix, this
; OOM'd (each tighten produced a negative int64 from `~x` which the
; propagator kept chasing, allocating trail entries unboundedly).
; bvnot 5 on an 8-bit value is 250, not -6.
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(declare-const y (_ BitVec 8))
(assert (= x #b00000101))
(assert (= y (bvnot x)))
(check-sat)
(get-value (y))
; Expected: sat, y = bv250
