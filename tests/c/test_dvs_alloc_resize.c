/* Test: dvs_alloc_resize — the hook path, the copy fallback for an
 * allocator with no resize member, and failure leaving the block intact. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dvs_alloc.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

/* An allocator with no resize member (as written before it existed), that
 * counts live bytes and can be told to fail. */
typedef struct {
    dvs_alloc_t base;
    long        live;
    int         fail;
} test_alloc_t;

static void *t_alloc(dvs_alloc_t *self, size_t sz) {
    test_alloc_t *t = (test_alloc_t *)self;
    if (t->fail) return NULL;
    t->live += (long)sz;
    return malloc(sz);
}
static void t_release(dvs_alloc_t *self, void *p, size_t sz) {
    test_alloc_t *t = (test_alloc_t *)self;
    t->live -= (long)sz;
    free(p);
}

static int filled(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)i) return 0;
    return 1;
}

int main(void) {
    test_alloc_t t = { { t_alloc, t_release }, 0, 0 };
    dvs_alloc_t *a = &t.base;

    CHECK(a->resize == NULL, "two-member initializer leaves resize NULL");

    unsigned char *p = DVS_RESIZE(a, NULL, 0, 16);
    CHECK(p != NULL && t.live == 16, "NULL ptr allocates");
    for (int i = 0; i < 16; i++) p[i] = (unsigned char)i;

    p = DVS_RESIZE(a, p, 16, 4096);
    CHECK(p != NULL && t.live == 4096, "fallback grows, old block released");
    CHECK(filled(p, 16), "grow keeps data");

    p = DVS_RESIZE(a, p, 4096, 8);
    CHECK(p != NULL && t.live == 8, "fallback shrinks");
    CHECK(filled(p, 8), "shrink keeps the prefix");

    t.fail = 1;
    unsigned char *q = DVS_RESIZE(a, p, 8, 1 << 20);
    CHECK(q == NULL, "failing allocator returns NULL");
    CHECK(t.live == 8 && filled(p, 8), "failed resize leaves the block intact");
    t.fail = 0;

    q = DVS_RESIZE(a, p, 8, 0);
    CHECK(q == NULL && t.live == 0, "new_size 0 releases");

    /* The default allocator has a resize hook (realloc). */
    CHECK(dvs_malloc_alloc.resize != NULL, "malloc allocator has resize");
    p = DVS_RESIZE(NULL, NULL, 0, 32);
    for (int i = 0; i < 32; i++) p[i] = (unsigned char)i;
    for (size_t n = 64; n <= (1u << 20); n *= 2) {
        p = DVS_RESIZE(NULL, p, n / 2, n);
        if (!p) break;
    }
    CHECK(p != NULL && filled(p, 32), "NULL allocator (malloc) grows, keeps data");
    DVS_RESIZE(NULL, p, 1u << 20, 0);

#ifdef DVS_DEBUG_ALWAYS_MOVE
    p = DVS_RESIZE(a, NULL, 0, 64);
    unsigned char *r = DVS_RESIZE(a, p, 64, 64);
    CHECK(r != NULL && r != p, "always-move: same-size resize moves");
    DVS_RESIZE(a, r, 64, 0);
#endif

    return failures ? 1 : 0;
}
