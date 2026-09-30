; Same problem as quickstart.smt2, with a seed: each seed picks a different
; solution, and the same seed always picks the same one.
(set-logic QF_BV)
(set-option :seed 7)
(declare-const addr (_ BitVec 32))
(declare-const len  (_ BitVec 8))
(assert (= ((_ extract 1 0) addr) #b00))
(assert (bvuge len #x01))
(assert (bvule len #x10))
(assert (bvult (bvadd addr ((_ zero_extend 24) len)) #x00000100))
(check-sat)
(get-value (addr len))
