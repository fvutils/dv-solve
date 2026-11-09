; Signed division by a variable power of two, q = a sdiv (1 << k), k <= 30,
; negative quotient window and signed remainder -3, 64 bits. Pure CDCL times out
; (>29 s); the 32-bit form solves in 0.05 s. Expect: sat.
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
(check-sat)
