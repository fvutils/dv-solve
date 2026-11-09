; AXI burst, 32-bit address, variable size: address aligned to 1<<size and the
; burst stays inside one 4 KB page. Shape as Verilator emits `1 << size`
; (shift amount zero-extended to the operand width). Expect: sat.
(set-logic QF_BV)
(declare-const addr (_ BitVec 32))
(declare-const size (_ BitVec 3))
(declare-const len (_ BitVec 8))
(define-fun sz32 () (_ BitVec 32) ((_ zero_extend 29) size))
(define-fun nbytes () (_ BitVec 32) (bvshl (bvadd ((_ zero_extend 24) len) #x00000001) sz32))
(assert (= (bvurem addr (bvshl #x00000001 sz32)) #x00000000))
(assert (bvule (bvadd ((_ zero_extend 20) ((_ extract 11 0) addr)) nbytes) #x00001000))
(assert (bvuge size #b010))
(assert (bvuge len #x0f))
(assert (bvuge addr #x80000000))
(check-sat)
