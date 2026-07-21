(set-logic QF_BV)
(declare-const b (_ BitVec 1))(declare-const v (_ BitVec 64))
(assert (bvuge (ite (= b (_ bv0 1)) (bvxor (_ bv18000000000000000000 64) (_ bv0 64)) v) v))
(check-sat)
