; As udiv_pow2set_32 at 64 bits, plus a product by the same power of two:
; a = c * b + r with r = b - 1 (remainder at its max). Expect: sat.
(set-logic QF_BV)
(declare-const a (_ BitVec 64))
(declare-const b (_ BitVec 64))
(declare-const c (_ BitVec 64))
(assert (or (= b #x0000000000000001) (= b #x0000000000000002) (= b #x0000000000000004) (= b #x0000000000000008)))
(assert (= c (bvudiv a b)))
(assert (bvugt c #x00000000000003e8))
(assert (bvult c #x00000000000007d0))
(assert (bvugt a #x0000000000001388))
(assert (= (bvurem a b) (bvsub b #x0000000000000001)))
(assert (bvugt (bvmul c b) #x0000000000003a98))
(check-sat)
