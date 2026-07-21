(set-logic QF_BV)
(declare-const x (_ BitVec 65))
(assert (= (bvadd x (_ bv1 65)) (_ bv0 65)))
(check-sat)
