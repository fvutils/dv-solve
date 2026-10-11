; get-value on terms, not just symbols (SMT-LIB 2.6 section 4.2.6). See
; docs/fw_hdl_requests.md, R1.
;
; Each response must echo the term and give its value in the model:
;   ((a #x5)) ((k #x6)) (((bvadd a #x1) #x6)) (((f a) #x6)) (((f #x2) #x3))
;   (((f (f a)) #x7)) (((bvneg a) #xb)) (((bvlshr a #x1) #x2))
;   (((bvxor a a a) #x5)) (((= a #x5) true))
; (z3 prints exactly this; dv-solve's #b form is equally fine.)
;
; dv-solve 10e8bdc instead prints `?` for every term, and the six terms its
; evaluator lacks come back as `#b0`, with no diagnostic and exit status 0.
; Those are: (f a), (f #x2), (f (f a)), bvneg, bvlshr and n-ary bvxor.
(set-logic QF_BV)
(set-option :produce-models true)
(declare-fun a () (_ BitVec 4))
(define-fun f ((x (_ BitVec 4))) (_ BitVec 4) (bvadd x #x1))
(define-fun k () (_ BitVec 4) (bvadd a #x1))
(assert (= a #x5))
(check-sat)
(get-value (a))
(get-value (k))
(get-value ((bvadd a #x1)))
(get-value ((f a)))
(get-value ((f #x2)))
(get-value ((f (f a))))
(get-value ((bvneg a)))
(get-value ((bvlshr a #x1)))
(get-value ((bvxor a a a)))
(get-value ((= a #x5)))
