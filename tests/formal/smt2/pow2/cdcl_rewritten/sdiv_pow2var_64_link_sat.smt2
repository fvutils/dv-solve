; sdiv_pow2var_64_sat plus the redundant link a = (q << k) + srem, srem = -3.
; Hand-applied target of structural_reasoning_design.md step 3 (CDCL);
; with the step-3 propagator changes, pure CDCL solves it in 0.00 s. Expect: sat.
(set-logic QF_BV)
(declare-const a (_ BitVec 64))
(declare-const k (_ BitVec 64))
(declare-const q (_ BitVec 64))
(define-fun b () (_ BitVec 64) (bvshl #x0000000000000001 k))
(assert (bvule k #x000000000000001e))
(assert (= q (bvsdiv a b)))
(assert (bvsge q (bvneg #x00000000000007d0)))
(assert (bvsle q (bvneg #x00000000000003e8)))
(assert (bvslt a (bvneg #x0000000010000000)))
(assert (= (bvsrem a b) (bvneg #x0000000000000003)))
(assert (= a (bvadd (bvshl q k) (bvneg #x0000000000000003))))
(check-sat)
