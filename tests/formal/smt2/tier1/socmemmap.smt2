(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const boot_base (_ BitVec 16))
(declare-const boot_size (_ BitVec 15))
(declare-const boot_end_addr (_ BitVec 16))
(declare-const sram_base (_ BitVec 16))
(declare-const sram_size (_ BitVec 15))
(declare-const sram_end_addr (_ BitVec 16))
(declare-const periph_base (_ BitVec 16))
(declare-const periph_size (_ BitVec 15))
(declare-const periph_end_addr (_ BitVec 16))
(declare-const dma_base (_ BitVec 16))
(declare-const dma_size (_ BitVec 15))
(declare-const dma_end_addr (_ BitVec 16))

(assert (bvuge boot_base (_ bv0 16)))
(assert (bvule boot_base (_ bv40959 16)))
(assert (bvuge boot_size (_ bv4096 15)))
(assert (bvule boot_size (_ bv24576 15)))
(assert (bvuge boot_end_addr (_ bv4096 16)))
(assert (bvule boot_end_addr (_ bv65535 16)))
(assert (bvuge sram_base (_ bv0 16)))
(assert (bvule sram_base (_ bv40959 16)))
(assert (bvuge sram_size (_ bv4096 15)))
(assert (bvule sram_size (_ bv24576 15)))
(assert (bvuge sram_end_addr (_ bv4096 16)))
(assert (bvule sram_end_addr (_ bv65535 16)))
(assert (bvuge periph_base (_ bv0 16)))
(assert (bvule periph_base (_ bv40959 16)))
(assert (bvuge periph_size (_ bv4096 15)))
(assert (bvule periph_size (_ bv24576 15)))
(assert (bvuge periph_end_addr (_ bv4096 16)))
(assert (bvule periph_end_addr (_ bv65535 16)))
(assert (bvuge dma_base (_ bv0 16)))
(assert (bvule dma_base (_ bv40959 16)))
(assert (bvuge dma_size (_ bv4096 15)))
(assert (bvule dma_size (_ bv24576 15)))
(assert (bvuge dma_end_addr (_ bv4096 16)))
(assert (bvule dma_end_addr (_ bv65535 16)))

(assert (= ((_ zero_extend 1) boot_end_addr) (bvadd ((_ zero_extend 1) boot_base) ((_ zero_extend 2) boot_size))))  ; c_boot_end
(assert (= ((_ zero_extend 1) sram_end_addr) (bvadd ((_ zero_extend 1) sram_base) ((_ zero_extend 2) sram_size))))  ; c_sram_end
(assert (= ((_ zero_extend 1) periph_end_addr) (bvadd ((_ zero_extend 1) periph_base) ((_ zero_extend 2) periph_size))))  ; c_periph_end
(assert (= ((_ zero_extend 1) dma_end_addr) (bvadd ((_ zero_extend 1) dma_base) ((_ zero_extend 2) dma_size))))  ; c_dma_end
(assert (bvule boot_end_addr sram_base))  ; c_boot_before_sram
(assert (bvule sram_end_addr periph_base))  ; c_sram_before_periph
(assert (bvule periph_end_addr dma_base))  ; c_periph_before_dma
(assert (bvule dma_end_addr (_ bv65535 16)))  ; c_fits_in_64k

(check-sat)
(get-value (boot_base boot_size boot_end_addr sram_base sram_size sram_end_addr periph_base periph_size periph_end_addr dma_base dma_size dma_end_addr))
