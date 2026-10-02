(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(assert (= x (_ bv18446744073709551621 8)))(assert (= x #x05))
(check-sat)
