(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= x (_ bv18446744073709551616 128)))(assert (distinct x #x00000000000000010000000000000000))
(check-sat)
