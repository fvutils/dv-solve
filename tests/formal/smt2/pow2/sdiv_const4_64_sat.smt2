; Signed division by the constant 4 at 64 bits (truncating toward zero):
; negative quotient window and a nonzero signed remainder. Expect: sat.
(set-logic QF_BV)
(declare-const a (_ BitVec 64))
(declare-const q (_ BitVec 64))
(assert (= q (bvsdiv a #x0000000000000004)))
(assert (bvsge q (bvneg #x00000000000007d0)))
(assert (bvsle q (bvneg #x00000000000003e8)))
(assert (= (bvsrem a #x0000000000000004) (bvneg #x0000000000000003)))
(assert (bvslt a (bvneg #x0000000000000fa0)))
(check-sat)
