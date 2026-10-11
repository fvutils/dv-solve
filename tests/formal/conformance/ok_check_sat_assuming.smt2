; check-sat-assuming
; expect: sat unsat sat
; exit: 0
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(declare-const p Bool)
(assert (= p (= x #x1)))
(check-sat-assuming (p))
(check-sat-assuming ((not p) p))
(check-sat)
