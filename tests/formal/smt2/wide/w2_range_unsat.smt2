(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (bvult #x7fffffffffffffffffffffffffffffff x))(assert (bvult x #x80000000000000000000000000000001))(assert (distinct x #x80000000000000000000000000000000))
(check-sat)
