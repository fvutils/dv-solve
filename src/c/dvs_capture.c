/**
 * dvs_capture.c -- DVS_CAPTURE=<file>: record handle images and calls
 * (format in dvs_capture.h). A call is built in its own buffer and written
 * whole under one lock, so calls on handles of different threads interleave
 * only as whole records.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dvs_capture.h"
#include "dvs_thread.h"

static dvs_mutex_t s_mutex = DVS_MUTEX_INIT;
static int s_state;          /* 0: not read yet; 1: on; -1: off */
static FILE *s_fp;
static uint32_t s_next_id;

#if defined(_MSC_VER)
#  define DVS__TLS __declspec(thread)
#else
#  define DVS__TLS _Thread_local
#endif
static DVS__TLS uint32_t s_depth;  /* API calls in progress on this thread */

static int capture_on(void) {
    if (s_state) return s_state > 0;
    dvs_mutex_lock(&s_mutex);
    if (!s_state) {
        const char *path = getenv("DVS_CAPTURE");
        s_fp = path && *path ? fopen(path, "wb") : NULL;
        if (path && *path && !s_fp) fprintf(stderr, "dv-solve: cannot write DVS_CAPTURE '%s'\n", path);
        if (s_fp) fwrite(DVSCAP_MAGIC, 1, sizeof DVSCAP_MAGIC - 1, s_fp);
        s_state = s_fp ? 1 : -1;
    }
    dvs_mutex_unlock(&s_mutex);
    return s_state > 0;
}

static void put(dvs_capbuf_t *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n) cap *= 2;
        uint8_t *d = realloc(b->d, cap);
        if (!d) abort();
        b->d = d;
        b->cap = cap;
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void put_u8(dvs_capbuf_t *b, uint8_t v) { put(b, &v, 1); }

static void put_u32(dvs_capbuf_t *b, uint32_t v) {
    uint8_t x[4];
    for (int i = 0; i < 4; i++) x[i] = (uint8_t)(v >> (8 * i));
    put(b, x, 4);
}

static void put_u64(dvs_capbuf_t *b, uint64_t v) {
    uint8_t x[8];
    for (int i = 0; i < 8; i++) x[i] = (uint8_t)(v >> (8 * i));
    put(b, x, 8);
}

static void emit(dvs_capbuf_t *b) {
    dvs_mutex_lock(&s_mutex);
    fwrite(b->d, 1, b->n, s_fp);
    fflush(s_fp);
    dvs_mutex_unlock(&s_mutex);
    free(b->d);
    memset(b, 0, sizeof *b);
}

size_t dvs_capture_ref_bytes(const dvs_ref_t *rf) {
    if (!rf->addr || rf->layout != DVS_REF_SCALAR || rf->width == 0 || rf->width > 64) return 0;
    return rf->width <= 8 ? 1 : rf->width <= 16 ? 2 : rf->width <= 32 ? 4 : 8;
}

static uint64_t ref_value(const dvs_ref_t *rf) {
    uint64_t u = 0;
    const uint8_t *a = rf->addr;
    for (size_t i = 0, n = dvs_capture_ref_bytes(rf); i < n; i++) u |= (uint64_t)a[i] << (8 * i);
    return u;
}

int dvs_capture_enter(void) { return s_depth++ == 0 && capture_on(); }

void dvs_capture_leave(void) { s_depth--; }

uint32_t dvs_capture_handle(const void *buf, size_t size, int canonical) {
    dvs_capbuf_t b = {0};
    dvs_mutex_lock(&s_mutex);
    uint32_t id = ++s_next_id;
    dvs_mutex_unlock(&s_mutex);
    put_u8(&b, 'H');
    put_u32(&b, id);
    put_u8(&b, (uint8_t)canonical);
    put_u64(&b, size);
    put(&b, buf, size);
    emit(&b);
    return id;
}

/* The values `kind` reads and writes besides the vars: the sizes, or the
 * elements of each array. */
static void put_arrays(dvs_capbuf_t *b, int kind, int inputs, const uint32_t *sizes,
                       const dvs_array_ref_t *arrays, const uint8_t *elem_bytes,
                       uint32_t n_arrays) {
    for (uint32_t a = 0; a < n_arrays; a++) {
        if (kind == DVS_CAP_SIZES) {
            put_u32(b, sizes[a]);
        } else if (kind == DVS_CAP_ARRAYS) {
            if (inputs) {
                put_u32(b, arrays[a].n_elems);
                put_u8(b, elem_bytes[a]);
            }
            if (arrays[a].n_elems) put(b, arrays[a].data, (size_t)arrays[a].n_elems * elem_bytes[a]);
        }
    }
}

void dvs_capture_begin(dvs_capbuf_t *b, uint32_t id, int kind, int modes, uint64_t seed,
                       const dvs_ref_t *refs, uint32_t n_vars, const uint32_t *sizes,
                       const dvs_array_ref_t *arrays, const uint8_t *elem_bytes,
                       uint32_t n_arrays) {
    memset(b, 0, sizeof *b);
    put_u8(b, 'C');
    put_u32(b, id);
    put_u8(b, (uint8_t)kind);
    put_u8(b, (uint8_t)modes);
    put_u64(b, seed);
    put_u32(b, refs ? n_vars : 0);
    put_u32(b, n_arrays);
    for (uint32_t v = 0; refs && v < n_vars; v++) {
        put_u8(b, refs[v].addr != NULL);
        put_u32(b, refs[v].width);
        put_u8(b, refs[v].is_signed);
        put_u8(b, refs[v].layout);
        put_u64(b, ref_value(&refs[v]));
    }
    put_arrays(b, kind, 1, sizes, arrays, elem_bytes, n_arrays);
}

void dvs_capture_end(dvs_capbuf_t *b, int kind, int rc, const dvs_ref_t *refs,
                     uint32_t n_vars, const uint32_t *sizes,
                     const dvs_array_ref_t *arrays, const uint8_t *elem_bytes,
                     uint32_t n_arrays) {
    put_u32(b, (uint32_t)rc);
    for (uint32_t v = 0; refs && v < n_vars; v++)
        if (dvs_capture_ref_bytes(&refs[v])) put_u64(b, ref_value(&refs[v]));
    put_arrays(b, kind, 0, sizes, arrays, elem_bytes, n_arrays);
    emit(b);
}

void dvs_capture_engine(uint32_t id, int engine) {
    dvs_capbuf_t b = {0};
    put_u8(&b, 'E');
    put_u32(&b, id);
    put_u32(&b, (uint32_t)engine);
    emit(&b);
}

void dvs_capture_parts(uint32_t id) {
    dvs_capbuf_t b = {0};
    put_u8(&b, 'P');
    put_u32(&b, id);
    emit(&b);
}
