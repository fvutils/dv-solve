(set-logic QF_BV)
(declare-const x (_ BitVec 128))(declare-const y (_ BitVec 128))
(assert (= x (_ bv1 128)))(assert (= (bvadd x y) (_ bv0 128)))
(check-sat)
