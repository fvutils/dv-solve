; (reset) clears the input-error state; the exit status still reports the error
; expect: error sat
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(frobnicate)
(reset)
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(assert (= x #x1))
(check-sat)
