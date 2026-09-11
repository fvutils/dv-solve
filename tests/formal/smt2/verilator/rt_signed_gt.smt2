; Regression (B11): signed bvsgt/bvsge lowered via MSB-flip xor. The variable
; side's xor is now materialised (so its binding compiles) and a constant operand
; is folded, so _bool_to_var reifies a clean var-vs-const inequality. Plus the
; bxor propagator does sign-bit-flip bounds propagation, so a conjunction of
; signed compares narrows x instead of blind-enumerating. Expect: sat (x in 5,6).
(set-logic QF_BV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(declare-fun x () (_ BitVec 32))
(assert (= #b1 (__Vbv (bvsgt x #x00000004))))
(assert (= #b1 (__Vbv (bvsgt x #x00000000))))
(assert (= #b1 (__Vbv (bvslt x #x00000007))))
(assert (= #b1 (__Vbv (bvsge x #x00000005))))
(check-sat)
