; the usual benchmark header (B79)
; expect: sat
; exit: 0
(set-info :smt-lib-version 2.6)
(set-info :status sat)
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(assert (= x #x1))
(check-sat)
