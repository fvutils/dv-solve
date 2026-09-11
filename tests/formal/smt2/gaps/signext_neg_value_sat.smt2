(set-logic QF_BV)
(declare-const x (_ BitVec 32))(declare-const y (_ BitVec 64))
(assert (= x #xffffffff))(assert (= y ((_ sign_extend 32) x)))(assert (= y #xffffffffffffffff))
(check-sat)
