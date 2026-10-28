; corpus for array.sum_partition from 210 captured calls, 1 distinct problems
; problem array.sum_partition#0 calls=210
; get-value (select a #x00000000)(select a #x00000001)(select a #x00000002)(select a #x00000003)(select b #x00000000)(select b #x00000001)(select b #x00000002)(select b #x00000003)
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (Array (_ BitVec 32) (_ BitVec 8)))
(declare-fun b () (Array (_ BitVec 32) (_ BitVec 8)))
(assert (= #b1 (__Vbv (= (bvadd (bvadd (bvadd (bvadd #b00000000000000000000000000000000 ((_ zero_extend 24) (select a #x00000000))) ((_ zero_extend 24) (select a #x00000001))) ((_ zero_extend 24) (select a #x00000002))) ((_ zero_extend 24) (select a #x00000003))) (bvadd (bvadd (bvadd (bvadd #b00000000000000000000000000000000 ((_ zero_extend 24) (select b #x00000000))) ((_ zero_extend 24) (select b #x00000001))) ((_ zero_extend 24) (select b #x00000002))) ((_ zero_extend 24) (select b #x00000003)))))))
(assert (= #b1 (__Vbv (= (bvadd (bvadd (bvadd (bvadd (bvadd #b00000000000000000000000000000000 ((_ zero_extend 24) (select a #x00000000))) ((_ zero_extend 24) (select a #x00000001))) ((_ zero_extend 24) (select a #x00000002))) ((_ zero_extend 24) (select a #x00000003))) (bvadd (bvadd (bvadd (bvadd #b00000000000000000000000000000000 ((_ zero_extend 24) (select b #x00000000))) ((_ zero_extend 24) (select b #x00000001))) ((_ zero_extend 24) (select b #x00000002))) ((_ zero_extend 24) (select b #x00000003)))) #x00000190))))
; end
