; corpus for array.sum_with from 210 captured calls, 1 distinct problems
; problem array.sum_with#0 calls=210
; get-value (select a #x00000000)(select a #x00000001)(select a #x00000002)(select a #x00000003)(select a #x00000004)(select a #x00000005)(select a #x00000006)(select a #x00000007)(select a #x00000008)(select a #x00000009)
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (Array (_ BitVec 32) (_ BitVec 4)))
(assert (= #b1 (__Vbv (= (bvadd (bvadd (bvadd (bvadd (bvadd (bvadd (bvadd (bvadd (bvadd (bvadd #b00000000000000000000000000000000 ((_ zero_extend 28) (select a #x00000000))) ((_ zero_extend 28) (select a #x00000001))) ((_ zero_extend 28) (select a #x00000002))) ((_ zero_extend 28) (select a #x00000003))) ((_ zero_extend 28) (select a #x00000004))) ((_ zero_extend 28) (select a #x00000005))) ((_ zero_extend 28) (select a #x00000006))) ((_ zero_extend 28) (select a #x00000007))) ((_ zero_extend 28) (select a #x00000008))) ((_ zero_extend 28) (select a #x00000009))) #x0000003c))))
; end
