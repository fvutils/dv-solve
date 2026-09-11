(set-logic QF_BV)
(declare-const b (_ BitVec 1))(declare-const v (_ BitVec 64))
(assert (bvuge (ite (= b (_ bv0 1)) (bvnot v) v) v))
(check-sat)
