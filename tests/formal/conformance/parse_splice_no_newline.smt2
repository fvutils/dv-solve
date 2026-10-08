; two scripts spliced without a newline (the fw-hdl printer bug): set-logic twice
; expect: sat error error unknown
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(assert (= x #x1))
(check-sat)(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(check-sat)
