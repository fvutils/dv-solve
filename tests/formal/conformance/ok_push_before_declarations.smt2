; a declaration and an assertion after a push/pop made before any declaration
; expect: sat
; exit: 0
; xfail: B83, push/pop before the first declaration leaves the next check-sat unknown
(set-logic QF_BV)
(push 1)
(pop 1)
(declare-const x (_ BitVec 8))
(assert (= x #xff))
(check-sat)
