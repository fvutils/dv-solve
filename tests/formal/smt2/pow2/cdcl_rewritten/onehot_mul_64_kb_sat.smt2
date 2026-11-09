; onehot_mul_64_sat with the change of variable b = 1 << kb, a * b written a << kb.
; Hand-applied target of structural_reasoning_design.md step 3 (CDCL);
; with the step-3 propagator changes, pure CDCL solves it in 0.00 s. Expect: sat.
(set-logic QF_BV)
(declare-const a (_ BitVec 64))
(declare-const b (_ BitVec 64))
(declare-const kb (_ BitVec 64))
(assert (bvule kb #x000000000000003f))
(assert (= b (bvshl #x0000000000000001 kb)))
(declare-const p (_ BitVec 64))
(assert (not (= b #x0000000000000000)))
(assert (= (bvand b (bvsub b #x0000000000000001)) #x0000000000000000))
(assert (= p (bvshl a kb)))
(assert (= ((_ extract 0 0) a) #b1))
(assert (bvugt a #x0000000000010000))
(assert (bvult a #x0000000100000000))
(assert (bvugt p #x0123456700000000 ))
(assert (bvult p #x0123456800000000 ))
(check-sat)
