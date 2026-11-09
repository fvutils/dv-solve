; Divisor from a power-of-two set (`b inside {1,2,4,8}`), 32 bits. Quotient and
; remainder both constrained; remainder forces b in {4,8}. Expect: sat.
(set-logic QF_BV)
(declare-const a (_ BitVec 32))
(declare-const b (_ BitVec 32))
(declare-const c (_ BitVec 32))
(assert (or (= b #x00000001) (= b #x00000002) (= b #x00000004) (= b #x00000008)))
(assert (= c (bvudiv a b)))
(assert (bvugt c #x000003e8))
(assert (bvult c #x000007d0))
(assert (bvugt a #x00001388))
(assert (= (bvurem a b) #x00000003))
(check-sat)
