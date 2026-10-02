#ifndef DVS_STACK_H
#define DVS_STACK_H

#include <stddef.h>
#include <stdint.h>
#include "dvs_block_alloc.h"

/**
 * dvs_stack_t — growable bump allocator backed by a chain of blocks.
 *
 * Memory is vended by bumping a pointer inside the current block.  When a
 * block is exhausted a fresh one is fetched from the dvs_block_alloc_t.
 *
 * Decision-level checkpointing is exposed via push/pop:
 *
 *   dvs_stack_mark_t mark = dvs_stack_push(stack);
 *   ... allocate N items ...
 *   dvs_stack_pop(stack, mark);   // returns all blocks allocated since
 *                                 // mark to block_alloc; O(blocks used)
 *
 * This is the backbone of the trail (domain-change undo log) and the
 * dynamic scratch space in the solver.
 */
typedef struct dvs_stack_s  dvs_stack_t;

/**
 * Opaque mark that records a stack checkpoint.
 * Obtain one with dvs_stack_push(); restore with dvs_stack_pop().
 */
typedef struct {
    void    *block;   /* block pointer at push time (may be NULL) */
    uint32_t intra;   /* intra-block bump offset at push time     */
} dvs_stack_mark_t;

/**
 * Create a new stack allocator.
 *
 * @param block_alloc  Source of blocks.  Must outlive the stack.
 * @return  New stack, or NULL on allocation failure.
 */
dvs_stack_t *dvs_stack_create(dvs_block_alloc_t *block_alloc);

/**
 * Destroy a stack, returning all currently held blocks to block_alloc
 * and freeing the stack struct itself.
 */
void dvs_stack_destroy(dvs_stack_t *stack);

/**
 * Bump-allocate `bytes` bytes with `align`-byte alignment.
 *
 * Fetches a new block from block_alloc when the current one is full.
 *
 * @return  Pointer to the allocated region, or NULL on allocation failure.
 */
void *dvs_stack_alloc(dvs_stack_t *stack, size_t bytes, size_t align);

/**
 * Record a checkpoint.  Returns a mark that can later be passed to
 * dvs_stack_pop() to undo all allocations made after this call.
 */
dvs_stack_mark_t dvs_stack_push(dvs_stack_t *stack);

/**
 * Restore the stack to the state at `mark`.
 *
 * Every block allocated after the checkpoint is returned to block_alloc.
 * The block that was current at push time is retained but its intra-block
 * pointer is reset to the saved position.
 */
void dvs_stack_pop(dvs_stack_t *stack, dvs_stack_mark_t mark);

/**
 * Return the number of blocks currently held by the stack
 * (useful for testing that pop returns blocks correctly).
 */
size_t dvs_stack_block_count(const dvs_stack_t *stack);

#endif /* DVS_STACK_H */
