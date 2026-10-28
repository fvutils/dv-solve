; corpus for dist.randc_enum from 210 captured calls, 39 distinct problems
; problem dist.randc_enum#0 calls=35
; get-value col v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun col () (_ BitVec 3))
(declare-fun v () (_ BitVec 8))
(assert (= #b1 (__Vbv (or (= col (_ bv0 3)) (= col (_ bv1 3)) (= col (_ bv2 3)) (= col (_ bv3 3)) (= col (_ bv4 3)) (= col (_ bv5 3))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= col #b000))) (__Vbool (__Vbv (bvult v #x32)))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= col #b010))) (__Vbool (__Vbv (bvugt v #xc8)))))))
; end
; problem dist.randc_enum#1 calls=20
; get-value col v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun col () (_ BitVec 3))
(declare-fun v () (_ BitVec 8))
(assert (= #b1 (__Vbv (or (= col (_ bv0 3)) (= col (_ bv1 3)) (= col (_ bv2 3)) (= col (_ bv3 3)) (= col (_ bv4 3)) (= col (_ bv5 3))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= col #b000))) (__Vbool (__Vbv (bvult v #x32)))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= col #b010))) (__Vbool (__Vbv (bvugt v #xc8)))))))
(assert (not (= col (_ bv1 3))))
(assert (not (= col (_ bv3 3))))
(assert (not (= col (_ bv4 3))))
(assert (not (= col (_ bv5 3))))
; end
; problem dist.randc_enum#2 calls=18
; get-value col v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun col () (_ BitVec 3))
(declare-fun v () (_ BitVec 8))
(assert (= #b1 (__Vbv (or (= col (_ bv0 3)) (= col (_ bv1 3)) (= col (_ bv2 3)) (= col (_ bv3 3)) (= col (_ bv4 3)) (= col (_ bv5 3))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= col #b000))) (__Vbool (__Vbv (bvult v #x32)))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= col #b010))) (__Vbool (__Vbv (bvugt v #xc8)))))))
(assert (not (= col (_ bv1 3))))
(assert (not (= col (_ bv2 3))))
(assert (not (= col (_ bv3 3))))
(assert (not (= col (_ bv4 3))))
(assert (not (= col (_ bv5 3))))
; end
; problem dist.randc_enum#3 calls=11
; get-value col v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun col () (_ BitVec 3))
(declare-fun v () (_ BitVec 8))
(assert (= #b1 (__Vbv (or (= col (_ bv0 3)) (= col (_ bv1 3)) (= col (_ bv2 3)) (= col (_ bv3 3)) (= col (_ bv4 3)) (= col (_ bv5 3))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= col #b000))) (__Vbool (__Vbv (bvult v #x32)))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= col #b010))) (__Vbool (__Vbv (bvugt v #xc8)))))))
(assert (not (= col (_ bv0 3))))
(assert (not (= col (_ bv1 3))))
(assert (not (= col (_ bv3 3))))
(assert (not (= col (_ bv4 3))))
(assert (not (= col (_ bv5 3))))
; end
