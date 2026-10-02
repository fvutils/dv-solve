(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= x #x0000000000000000fffffffffffffffe))(assert (distinct ((_ extract 127 64) x) #x0000000000000000))
(check-sat)
