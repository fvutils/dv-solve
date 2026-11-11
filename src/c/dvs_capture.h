/**
 * dvs_capture.h -- DVS_CAPTURE=<file>: record the problem images of the
 * handles and the calls on them, for dvs_capture_replay
 * (tests/c/dvs_capture_replay.c) to solve again without the client.
 *
 * The file is binary, little-endian, after the magic DVSCAP_MAGIC. Records:
 *
 *   'H' u32 id, u8 canonical, u64 size,     a handle's image (dvs_handle_new),
 *       size bytes                          and its DVS_CANONICAL
 *   'E' u32 id, u32 engine                  dvs_handle_set_engine (also from
 *                                           DV_ENGINE)
 *   'C' u32 id, u8 kind, u8 modes, u64 seed, a call on handle `id` (`modes`:
 *       u32 n_vars, u32 n_arrays,           given non-NULL)
 *       inputs, i32 rc, outputs
 *   'P' u32 id                              a dvs_randomize_parts* call rooted
 *                                           at handle `id` (not recorded yet)
 *
 * The inputs of a call are, per var: u8 addr (non-NULL), u32 width, u8
 * is_signed, u8 layout, u64 value (the ref's bytes, zero-extended, when it is
 * a valid ref); for DVS_CAP_SIZES a u32 size
 * per array; for DVS_CAP_ARRAYS per array u32 n_elems, u8 elem_bytes and the
 * elements' bytes. dvs_handle_import_state (internal, for tests) is not
 * recorded: a handle given state by it replays differently. The outputs are
 * the same values after the call: per valid ref its u64 value, then the
 * sizes or the elements.
 */
#ifndef DVS_CAPTURE_H
#define DVS_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include "dv_solve.h"

#define DVSCAP_MAGIC "DVSCAP1\n"

enum { DVS_CAP_RANDOMIZE = 0, DVS_CAP_SIZES = 1, DVS_CAP_ARRAYS = 2 };

/** One call being recorded: its inputs, then its outputs. */
typedef struct {
    uint8_t *d;
    size_t   n, cap;
} dvs_capbuf_t;

/** Begin an API call on this thread: return 1 if it is to be recorded,
 *  DVS_CAPTURE being set and the call not made inside another API call (by
 *  dv-solve itself, on its own handles). Each is paired with
 *  dvs_capture_leave(). */
int dvs_capture_enter(void);
void dvs_capture_leave(void);

/** Record a handle's image and canonical mode; return its id (from 1). */
uint32_t dvs_capture_handle(const void *buf, size_t size, int canonical);

/** Record an engine set on handle `id`. */
void dvs_capture_engine(uint32_t id, int engine);

/** Record the inputs of a call on handle `id` into `b`. `sizes` is read for
 *  DVS_CAP_SIZES, `arrays` and `elem_bytes` for DVS_CAP_ARRAYS. */
void dvs_capture_begin(dvs_capbuf_t *b, uint32_t id, int kind, int modes, uint64_t seed,
                       const dvs_ref_t *refs, uint32_t n_vars, const uint32_t *sizes,
                       const dvs_array_ref_t *arrays, const uint8_t *elem_bytes,
                       uint32_t n_arrays);

/** The bytes dvs_randomize reads or writes through a ref: 0 for a NULL or
 *  invalid one (the call then fails before it reads any). */
size_t dvs_capture_ref_bytes(const dvs_ref_t *rf);

/** Add the call's result and outputs to `b`, write it and free `b`. */
void dvs_capture_end(dvs_capbuf_t *b, int kind, int rc, const dvs_ref_t *refs,
                     uint32_t n_vars, const uint32_t *sizes,
                     const dvs_array_ref_t *arrays, const uint8_t *elem_bytes,
                     uint32_t n_arrays);

/** Record a dvs_randomize_parts* call rooted at handle `id`. */
void dvs_capture_parts(uint32_t id);

#endif
