; :print-success answers success to every command with no other response
; expect: success success success success success sat success
; exit: 0
(set-option :print-success true)
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(assert (= x #x1))
(push 1)
(check-sat)
(exit)
