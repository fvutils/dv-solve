; push/pop: constraints asserted inside a scope are discarded by pop.
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(assert (bvugt x #x10))
(push 1)
(assert (bvult x #x10))       ; contradicts x > 0x10
(check-sat)                   ; unsat
(pop 1)
(assert (= x #x20))
(check-sat)                   ; sat again
(get-value (x))
