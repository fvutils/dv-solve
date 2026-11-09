; mul_pow2var_64_sat with rewrite B: a * (1 << k) written a << k.
; Hand-applied target of structural_reasoning_design.md step 3 (CDCL);
; with the step-3 propagator changes, pure CDCL solves it in 0.00 s. Expect: sat.
(set-logic QF_BV)
(declare-const a (_ BitVec 64))
(declare-const k (_ BitVec 64))
(declare-const p (_ BitVec 64))
(assert (bvule k #x000000000000003f))
(assert (= p (bvshl a k)))
(assert (= ((_ extract 0 0) a) #b1))
(assert (bvugt a #x0000000000010000))
(assert (bvugt p #x0123456700000000 ))
(assert (bvult p #x0123456800000000 ))
(assert (bvuge k #x0000000000000018))
(check-sat)
