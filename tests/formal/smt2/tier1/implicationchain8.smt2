(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const a (_ BitVec 7))
(declare-const b (_ BitVec 7))
(declare-const c (_ BitVec 7))
(declare-const d (_ BitVec 7))
(declare-const e (_ BitVec 7))
(declare-const f (_ BitVec 7))
(declare-const g (_ BitVec 7))
(declare-const h (_ BitVec 7))

(assert (bvuge a (_ bv0 7)))
(assert (bvule a (_ bv100 7)))
(assert (bvuge b (_ bv0 7)))
(assert (bvule b (_ bv100 7)))
(assert (bvuge c (_ bv0 7)))
(assert (bvule c (_ bv100 7)))
(assert (bvuge d (_ bv0 7)))
(assert (bvule d (_ bv100 7)))
(assert (bvuge e (_ bv0 7)))
(assert (bvule e (_ bv100 7)))
(assert (bvuge f (_ bv0 7)))
(assert (bvule f (_ bv100 7)))
(assert (bvuge g (_ bv0 7)))
(assert (bvule g (_ bv100 7)))
(assert (bvuge h (_ bv0 7)))
(assert (bvule h (_ bv100 7)))

(assert (or (bvule a (_ bv50 7)) (bvult b (_ bv30 7))))  ; c_ab
(assert (or (bvuge b (_ bv30 7)) (bvugt c (_ bv60 7))))  ; c_bc
(assert (or (bvule c (_ bv60 7)) (bvult d (_ bv20 7))))  ; c_cd
(assert (or (bvuge d (_ bv20 7)) (bvugt e (_ bv70 7))))  ; c_de
(assert (or (bvule e (_ bv70 7)) (bvult f (_ bv25 7))))  ; c_ef
(assert (or (bvuge f (_ bv25 7)) (bvugt g (_ bv80 7))))  ; c_fg
(assert (or (bvule g (_ bv80 7)) (bvult h (_ bv15 7))))  ; c_gh

(check-sat)
(get-value (a b c d e f g h))
