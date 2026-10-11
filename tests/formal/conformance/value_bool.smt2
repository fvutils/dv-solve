; get-value on bool terms, every value checked against z3
; expect: sat values
; exit: 0
; values: z3
(set-logic QF_BV)
(declare-const a (_ BitVec 8))
(declare-const b (_ BitVec 8))
(declare-const c (_ BitVec 8))
(declare-const p Bool)
(assert (= a #xb5))
(assert (= b #x2c))
(assert (= c #x00))
(assert p)
(check-sat)
(get-value (p (not p) (and p (bvult a b)) (or p false) (xor p true) (=> p (= a b)) (ite p a b) (ite (bvult a b) #x01 #x02)))
