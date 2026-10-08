; a 300-bit sort is beyond our maximum (192): an honest unknown, and no model
; expect: unknown error
; exit: 4
(set-logic QF_BV)
(declare-const a (_ BitVec 300))
(assert (= ((_ extract 3 0) a) #x5))
(check-sat)
(get-value (((_ extract 3 0) a)))
