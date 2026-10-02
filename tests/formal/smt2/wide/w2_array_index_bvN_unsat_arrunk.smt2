(set-logic QF_ABV)
(declare-const a (Array (_ BitVec 128) (_ BitVec 8)))
(assert (= (select a #x00000000000000010000000000000005) #x01))(assert (= (select a (_ bv18446744073709551621 128)) #x02))
(check-sat)
