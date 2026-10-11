; a term we cannot evaluate: an error, and the session goes on
; expect: sat error sat
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(assert (= x #x1))
(check-sat)
(get-value ((bv2nat x)))
(check-sat)
