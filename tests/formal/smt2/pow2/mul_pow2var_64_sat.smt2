; p = a * (1 << k), a odd, p in a narrow window, 64 bits: k must equal p's
; trailing-zero count. Pure CDCL times out (>29 s); default solves via bit-blast. Expect: sat.
(set-logic QF_BV)
(declare-const a (_ BitVec 64))
(declare-const k (_ BitVec 64))
(declare-const p (_ BitVec 64))
(assert (bvule k #x000000000000003f))
(assert (= p (bvmul a (bvshl #x0000000000000001 k))))
(assert (= ((_ extract 0 0) a) #b1))
(assert (bvugt a #x0000000000010000))
(assert (bvugt p #x0123456700000000 ))
(assert (bvult p #x0123456800000000 ))
(assert (bvuge k #x0000000000000018))
(check-sat)
