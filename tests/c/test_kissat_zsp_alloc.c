/* Phase B.1 step 2 test: verify kissat routes allocations through
 * zsp_alloc_t when one is supplied. Builds a counting allocator,
 * runs a trivial SAT through zsp_sat_new(counting), and checks that
 * every byte allocated is also freed via the same allocator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zsp_alloc.h"
#include "zsp_sat.h"

typedef struct {
    zsp_alloc_t base;
    size_t      n_allocs;
    size_t      n_frees;
    size_t      bytes_alloced;
    size_t      bytes_freed;
} counting_alloc_t;

static void *count_alloc(zsp_alloc_t *self, size_t sz) {
    counting_alloc_t *c = (counting_alloc_t *)self;
    c->n_allocs++;
    c->bytes_alloced += sz;
    return malloc(sz);
}
static void count_release(zsp_alloc_t *self, void *p, size_t sz) {
    counting_alloc_t *c = (counting_alloc_t *)self;
    c->n_frees++;
    c->bytes_freed += sz;
    free(p);
}

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

int main(void) {
    counting_alloc_t ca;
    memset(&ca, 0, sizeof(ca));
    ca.base.alloc = count_alloc;
    ca.base.release = count_release;

    zsp_sat_t *s = zsp_sat_new(&ca.base);
    CHECK(s != NULL, "zsp_sat_new succeeded with custom alloc");
    CHECK(ca.n_allocs > 0, "kissat performed allocations through our allocator");

    /* Drive a trivial SAT problem: (a v b) ∧ (¬a v ¬b). SAT, model
     * a XOR b. */
    zsp_sat_add_binary(s, 1, 2);
    zsp_sat_add_binary(s, -1, -2);
    int rc = zsp_sat_solve(s);
    CHECK(rc == ZSP_SAT_SAT, "trivial SAT returns SAT");

    size_t allocs_before_release = ca.n_allocs;
    size_t frees_before_release  = ca.n_frees;
    printf("       in-flight: %zu allocs, %zu frees, bytes %zu / %zu\n",
           ca.n_allocs, ca.n_frees, ca.bytes_alloced, ca.bytes_freed);

    zsp_sat_free(s);

    /* Critical invariant: after release, every byte we handed out via
     * our allocator must have come back via our allocator. */
    CHECK(ca.n_frees >= allocs_before_release,
          "all allocations freed through our allocator");
    CHECK(ca.n_frees > frees_before_release,
          "release routed kissat's frees through our allocator");
    CHECK(ca.bytes_freed == ca.bytes_alloced,
          "byte counts balance — no leaks via libc");
    printf("       after release: %zu allocs, %zu frees, bytes %zu / %zu\n",
           ca.n_allocs, ca.n_frees, ca.bytes_alloced, ca.bytes_freed);

    return failures ? 1 : 0;
}
