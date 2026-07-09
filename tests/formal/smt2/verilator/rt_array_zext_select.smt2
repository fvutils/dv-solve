; Regression: zero_extend of an array-select must NOT be const-folded to 0.
; The select handler once tagged element VARIABLES as leaf_kind=const, so
; `(= ((_ zero_extend N) (select a i)) k)` folded to `0 == k` -> wrong unsat.
(set-logic QF_ABV)
(declare-fun a () (Array (_ BitVec 32) (_ BitVec 8)))
(assert (= ((_ zero_extend 24) (select a #x00000000)) #x00000005))
(assert (distinct (select a #x00000000) (select a #x00000001)))
(check-sat)
