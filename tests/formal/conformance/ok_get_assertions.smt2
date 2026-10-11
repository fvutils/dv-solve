; get-assertions lists the assertions in scope
; expect: assertions
; exit: 2
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(assert (= x #x1))
(push 1)
(assert (= y #x2))
(pop 1)
(get-assertions)
