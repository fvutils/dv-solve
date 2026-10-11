; a 300-bit sort (wider than the 192-bit maximum before piece 6.6): solved,
; and its values checked against z3
; expect: sat values
; exit: 0
; values: z3
(set-logic QF_BV)
(declare-const a (_ BitVec 300))
(assert (= ((_ extract 3 0) a) #x5))
(assert (= ((_ extract 299 296) a) #xa))
(check-sat)
(get-value (((_ extract 3 0) a) ((_ extract 299 296) a)))
