(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= x (concat (_ bv1 64) (_ bv2 64))))
(check-sat)
