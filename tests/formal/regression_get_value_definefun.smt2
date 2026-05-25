; Regression for (get-value (X)) where X is a define-fun macro
; (Phase 10 item 4). yosys-smtbmc cover loop queries cover-firing
; conditions this way; previously dv-solve responded with empty "()"
; which crashed smtio's parser with IndexError.
;
; Fix: get-value handler now falls back to a Sexpr evaluator that
; expands define-fun bodies and looks up the model values of the
; underlying declared variables.
;
; Expected: sat, then ((|c1| (_ bv1 1))), then ((|c2| (_ bv0 1))).
(set-logic QF_BV)
(declare-fun a () (_ BitVec 4))
(assert (= a #b0101))
(define-fun c1 () Bool (and (= ((_ extract 0 0) a) #b1) (= ((_ extract 2 2) a) #b1)))
(define-fun c2 () (_ BitVec 1) (ite (= ((_ extract 1 1) a) #b1) #b1 #b0))
(check-sat)
(get-value (c1))
(get-value (c2))
