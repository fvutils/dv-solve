; a string never closed swallows the rest
; expect: error
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(echo "abc)
(check-sat)
