; Regression for "clause unit-propagation entries treated as decisions"
; bug (Phase 2). The bvor here generates a learnt clause that
; unit-propagates a bound on r. The conflict analyzer used to see the
; resulting trail entry's prop_ref == EXPR_NULL and treat it as a
; decision — emitting a 1-literal learnt clause that forced an
; over-strong constraint at level 0 and incorrectly produced UNSAT.
;
; Expected after fix: sat (e.g. a=2, b=2, r=2; bvor 0010 0010 = 0010).
;
; Fix: clause_propagate now stamps TRAIL_FLAG_FROM_CLAUSE on the trail
; entry and stores the clause index in prop_ref. lcg_analyze_conflict
; checks the flag and resolves through the clause's other literals
; (which were false at unit-prop time) instead of treating it as a
; decision.
(set-logic QF_BV)
(declare-const a (_ BitVec 4))
(declare-const b (_ BitVec 4))
(declare-const r (_ BitVec 4))
(assert (bvuge a (_ bv2 4)))
(assert (bvuge b (_ bv1 4)))
(assert (= r (bvor a b)))
(assert (bvule r (_ bv2 4)))
(check-sat)
