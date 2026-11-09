; AXI burst, 64-bit address: bytes = (len+1) * (1<<size) bounded, address aligned
; to 1<<size with the alignment written as a modulo, burst end inside one 4 KB
; page, and the address high in the space. Expect: sat.
(set-logic QF_BV)
(declare-const addr (_ BitVec 64))
(declare-const size (_ BitVec 64))
(declare-const len (_ BitVec 64))
(define-fun b () (_ BitVec 64) (bvshl #x0000000000000001 size))
(define-fun nbytes () (_ BitVec 64) (bvmul (bvadd len #x0000000000000001) b))
(assert (bvule size #x0000000000000007))
(assert (bvuge size #x0000000000000003))
(assert (bvule len #x00000000000000ff))
(assert (bvuge nbytes #x0000000000000200))
(assert (= (bvurem addr b) #x0000000000000000))
(assert (bvule (bvadd (bvand addr #x0000000000000fff) nbytes) #x0000000000001000))
(assert (bvuge addr #x0000010000000000))
(assert (bvult addr #x0000020000000000))
(check-sat)
