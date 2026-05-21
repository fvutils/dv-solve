(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const a (_ BitVec 6))
(declare-const b (_ BitVec 6))
(declare-const c (_ BitVec 6))
(declare-const d (_ BitVec 6))
(declare-const e (_ BitVec 6))
(declare-const f (_ BitVec 6))
(declare-const g (_ BitVec 6))
(declare-const h (_ BitVec 6))

(assert (bvuge a (_ bv1 6)))
(assert (bvule a (_ bv50 6)))
(assert (bvuge b (_ bv1 6)))
(assert (bvule b (_ bv50 6)))
(assert (bvuge c (_ bv1 6)))
(assert (bvule c (_ bv50 6)))
(assert (bvuge d (_ bv1 6)))
(assert (bvule d (_ bv50 6)))
(assert (bvuge e (_ bv1 6)))
(assert (bvule e (_ bv50 6)))
(assert (bvuge f (_ bv1 6)))
(assert (bvule f (_ bv50 6)))
(assert (bvuge g (_ bv1 6)))
(assert (bvule g (_ bv50 6)))
(assert (bvuge h (_ bv1 6)))
(assert (bvule h (_ bv50 6)))

(assert (bvult a b))  ; c_chain
(assert (bvult b c))  ; c_chain
(assert (bvult c d))  ; c_chain
(assert (bvult d e))  ; c_chain
(assert (bvult e f))  ; c_chain
(assert (bvult f g))  ; c_chain
(assert (bvult g h))  ; c_chain

(check-sat)
(get-value (a b c d e f g h))
