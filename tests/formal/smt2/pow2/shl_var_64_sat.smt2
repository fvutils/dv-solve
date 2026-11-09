; mul_pow2var_64 after rewrite B: p = a << k in place of a * (1 << k).
; Control: pure CDCL still times out, so the shift rewrite alone does not
; help CDCL here; known bits / congruence (plan step 3) are needed. Expect: sat.
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
