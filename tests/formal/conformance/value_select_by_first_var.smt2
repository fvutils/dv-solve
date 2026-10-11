; a symbolic select indexed by the first declared variable (B85: ref 0 -> unknown)
; expect: sat values unsat
; exit: 1
; values: z3
(set-logic QF_ABV)
(declare-const x (_ BitVec 4))
(declare-const m (Array (_ BitVec 4) (_ BitVec 8)))
(declare-const v (_ BitVec 8))
(assert (= x #x2))
(assert (= (select m #x2) #x04))
(assert (= v (select m x)))
(check-sat)
(get-value (v (select m x)))
(assert (not (= v #x04)))
(check-sat)
