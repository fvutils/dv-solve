; Verilator lowering of `b == 2**n` with `int n` (signed exponent):
; ite(n >= 0, 1 << n, 0). Then addr aligned to b by modulo. Expect: sat.
(set-logic QF_BV)
(declare-const n (_ BitVec 32))
(declare-const b (_ BitVec 32))
(declare-const addr (_ BitVec 32))
(assert (bvsge n #x00000000))
(assert (bvsle n #x0000000c))
(assert (= b (ite (bvsge n #x00000000) (bvshl #x00000001 n) #x00000000)))
(assert (= (bvurem addr b) #x00000000))
(assert (bvugt addr #x10000000))
(assert (bvult addr #x10001000))
(assert (bvuge b #x00000100))
(check-sat)
