(set-logic QF_ABV)
(declare-const a (Array (_ BitVec 4) (_ BitVec 8)))(declare-const i (_ BitVec 4))
(assert (= (select a i) #x2a))(assert (= (select a i) #x2b))
(check-sat)
