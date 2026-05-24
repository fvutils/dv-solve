; Regression for two bugs found in incremental SMT2 mode while
; integrating dv-solve as a yosys-smtbmc backend:
;
; 1. _bool_to_var didn't accept EXTRACT shapes, so an
;    (or (= ((_ extract H L) v) k) ...) constraint compiled to
;    nothing and search returned wrong unsat.
;
; 2. solver_add_constraint walked the builder's LIFO vars_head
;    and gated initialisation on `id >= ctx->n_vars`. With LIFO
;    order, the highest new id bumps n_vars first and the lower
;    new ids fall below the threshold — skipped, never inited,
;    width stays 0, lo=hi=0.
;
; 3. _fresh_aux used fe->n_vars as the next ID, but the backend
;    compile path can allocate extra internal aux vars (constant
;    aux, ITE result, reification) that fe->n_vars doesn't know
;    about. The new aux IDs collide with existing backend slots.
;    Fix: skip past ctx->n_vars when picking the next id.
;
; Expected: sat / sat (the second OR is trivially satisfied).
(set-logic QF_BV)
(declare-fun u () (_ BitVec 1))
(assert (= ((_ extract 0 0) u) #b1))
(check-sat)
(assert (or (= ((_ extract 0 0) u) #b1) (= ((_ extract 0 0) u) #b1)))
(check-sat)
