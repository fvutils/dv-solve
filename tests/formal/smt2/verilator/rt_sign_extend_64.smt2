; Regression B1: sign_extend to exactly 64 bits was wrongly `unsat` (the native
; CDCL compile of `r == sign_extend(a)` bounded a 64-bit unsigned result var
; with a negative lower bound -> empty domain). Fixed by force-routing
; sign_extend-to->=64 to bitblast (which lowers it exactly) + having the bitblast
; path skip the CDCL compile. x=5 -> sext = 5.
(set-logic QF_BV)
(declare-fun x () (_ BitVec 32))
(assert (= ((_ sign_extend 32) x) (_ bv5 64)))
(check-sat)
