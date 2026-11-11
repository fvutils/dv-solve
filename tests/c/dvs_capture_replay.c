/**
 * dvs_capture_replay.c -- solve a DVS_CAPTURE file (src/c/dvs_capture.h)
 * again through the handle API, without the client that made it.
 *
 * Each pass opens the captured images as fresh handles and makes the
 * captured calls on them in order. A handle carries state from call to call
 * (kept clauses, randc histories), and a call's result depends only on that
 * state, the seed and the STATE values, so the first pass must reproduce
 * every result and value: it checks them. The time of each call is that of
 * the dvs_randomize* call alone; per handle and overall, the best pass is
 * reported, without the first --warmup calls of the capture (a simulator's
 * warm-up randomize() calls, times the calls each makes).
 *
 * The engine set on a handle (dvs_handle_set_engine or DV_ENGINE) is in the
 * capture and set again; DV_ENGINE given to the replay applies to handles
 * that had none. dvs_randomize_parts* calls are not in the capture yet ('P' records): they
 * are counted, and a handle used by one may go on differently.
 *
 *   dvs_capture_replay [--passes K] [--warmup N] [--verbose] capture.dvscap
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dv_solve.h"
#include "dvs_capture.h"

typedef struct {
    const uint8_t *p, *end;
} rd_t;

static void need(rd_t *r, size_t n) {
    if ((size_t)(r->end - r->p) < n) {
        fprintf(stderr, "dvs_capture_replay: truncated capture\n");
        exit(2);
    }
}
static uint8_t rd_u8(rd_t *r) { need(r, 1); return *r->p++; }
static uint32_t rd_u32(rd_t *r) {
    need(r, 4);
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)r->p[i] << (8 * i);
    r->p += 4;
    return v;
}
static uint64_t rd_u64(rd_t *r) {
    need(r, 8);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)r->p[i] << (8 * i);
    r->p += 8;
    return v;
}
static const uint8_t *rd_bytes(rd_t *r, size_t n) {
    need(r, n);
    const uint8_t *p = r->p;
    r->p += n;
    return p;
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* Per handle id: its handle in this pass and its counts. */
typedef struct {
    dvs_handle_t *h;
    uint32_t calls, timed;
    double ns, best_ns;
} site_t;

static site_t *sites;
static uint32_t n_sites;

static site_t *site(uint32_t id) {
    if (id >= n_sites) {
        uint32_t n = id + 16;
        sites = realloc(sites, n * sizeof *sites);
        memset(sites + n_sites, 0, (n - n_sites) * sizeof *sites);
        n_sites = n;
    }
    return &sites[id];
}

static uint64_t le_value(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint64_t n_mismatch;
static int verbose;

static void mismatch(uint64_t call, uint32_t id, const char *what, uint64_t want, uint64_t got) {
    if (n_mismatch++ < 20 || verbose)
        fprintf(stderr, "mismatch: call %llu (handle %u) %s: captured 0x%llx, replayed 0x%llx\n",
                (unsigned long long)call, id, what, (unsigned long long)want,
                (unsigned long long)got);
}

/* Replay one 'C' record (after its tag); check it when `check`. */
static void replay_call(rd_t *r, uint64_t call, int check, uint32_t warmup) {
    uint32_t id = rd_u32(r);
    int kind = rd_u8(r);
    int modes = rd_u8(r);
    static const uint32_t any_modes[1];  /* the call had modes: it fails alike */
    uint64_t seed = rd_u64(r);
    uint32_t n_vars = rd_u32(r), n_arrays = rd_u32(r);
    dvs_ref_t *refs = calloc(n_vars ? n_vars : 1, sizeof *refs);
    uint64_t *vals = calloc(n_vars ? n_vars : 1, sizeof *vals);
    uint32_t *sizes = calloc(n_arrays ? n_arrays : 1, sizeof *sizes);
    dvs_array_ref_t *arrays = calloc(n_arrays ? n_arrays : 1, sizeof *arrays);
    uint8_t *eb = calloc(n_arrays ? n_arrays : 1, 1);
    for (uint32_t v = 0; v < n_vars; v++) {
        int addr = rd_u8(r);
        refs[v].width = rd_u32(r);
        refs[v].is_signed = rd_u8(r);
        refs[v].layout = rd_u8(r);
        vals[v] = rd_u64(r);
        if (addr) refs[v].addr = &vals[v];  /* little-endian host */
    }
    for (uint32_t a = 0; a < n_arrays; a++) {
        if (kind == DVS_CAP_SIZES) {
            sizes[a] = rd_u32(r);
        } else if (kind == DVS_CAP_ARRAYS) {
            arrays[a].n_elems = rd_u32(r);
            eb[a] = rd_u8(r);
            size_t n = (size_t)arrays[a].n_elems * eb[a];
            if (n) {
                arrays[a].data = malloc(n);
                memcpy(arrays[a].data, rd_bytes(r, n), n);
            }
        }
    }
    site_t *s = site(id);
    if (!s->h) {
        fprintf(stderr, "dvs_capture_replay: call %llu on handle %u, not opened\n",
                (unsigned long long)call, id);
        exit(2);
    }
    double t0 = now_ns();
    int rc = kind == DVS_CAP_SIZES    ? dvs_randomize_sizes(s->h, n_vars ? refs : NULL, sizes, seed)
             : kind == DVS_CAP_ARRAYS ? dvs_randomize_arrays(s->h, n_vars ? refs : NULL, arrays, modes ? any_modes : NULL, seed)
                                      : dvs_randomize(s->h, n_vars ? refs : NULL, modes ? any_modes : NULL, seed);
    double t = now_ns() - t0;
    s->calls++;
    if (call >= warmup) {
        s->timed++;
        s->ns += t;
    }
    int want_rc = (int)rd_u32(r);
    if (check && rc != want_rc) mismatch(call, id, "result", (uint64_t)want_rc, (uint64_t)rc);
    for (uint32_t v = 0; v < n_vars; v++) {
        size_t n = dvs_capture_ref_bytes(&refs[v]);
        if (!n) continue;
        uint64_t want = rd_u64(r);
        uint64_t got = le_value((const uint8_t *)&vals[v], n);
        if (check && want != got) {
            char what[32];
            snprintf(what, sizeof what, "v%u", v);
            mismatch(call, id, what, want, got);
        }
    }
    for (uint32_t a = 0; a < n_arrays; a++) {
        if (kind == DVS_CAP_SIZES) {
            uint32_t want = rd_u32(r);
            if (check && want != sizes[a]) mismatch(call, id, "size", want, sizes[a]);
        } else if (kind == DVS_CAP_ARRAYS) {
            size_t n = (size_t)arrays[a].n_elems * eb[a];
            const uint8_t *want = rd_bytes(r, n);
            if (check && n && memcmp(want, arrays[a].data, n)) {
                for (uint32_t e = 0; e < arrays[a].n_elems; e++) {
                    uint64_t w = le_value(want + (size_t)e * eb[a], eb[a]);
                    uint64_t g = le_value((uint8_t *)arrays[a].data + (size_t)e * eb[a], eb[a]);
                    if (w != g) {
                        char what[48];
                        snprintf(what, sizeof what, "array %u[%u]", a, e);
                        mismatch(call, id, what, w, g);
                        break;
                    }
                }
            }
        }
        free(arrays[a].data);
    }
    free(refs);
    free(vals);
    free(sizes);
    free(arrays);
    free(eb);
}

int main(int argc, char **argv) {
    int passes = 3;
    uint32_t warmup = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--passes") && i + 1 < argc) passes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else {
            fprintf(stderr, "usage: dvs_capture_replay [--passes K] [--warmup N] [--verbose] FILE\n");
            return 2;
        }
    }
    if (!path || passes < 1) {
        fprintf(stderr, "usage: dvs_capture_replay [--passes K] [--warmup N] [--verbose] FILE\n");
        return 2;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "dvs_capture_replay: cannot read '%s'\n", path);
        return 2;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc(size > 0 ? (size_t)size : 1);
    if (!buf || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "dvs_capture_replay: cannot read '%s'\n", path);
        return 2;
    }
    fclose(fp);
    size_t ml = sizeof DVSCAP_MAGIC - 1;
    if ((size_t)size < ml || memcmp(buf, DVSCAP_MAGIC, ml)) {
        fprintf(stderr, "dvs_capture_replay: '%s' is not a DVS_CAPTURE file\n", path);
        return 2;
    }

    uint64_t n_calls = 0, n_parts = 0;
    uint32_t n_handles = 0;
    double best_total = -1;
    uint64_t timed_total = 0;
    for (int pass = 0; pass < passes; pass++) {
        for (uint32_t i = 0; i < n_sites; i++) {
            dvs_handle_free(sites[i].h);
            sites[i].h = NULL;
            sites[i].calls = sites[i].timed = 0;
            sites[i].ns = 0;
        }
        rd_t r = {buf + ml, buf + size};
        n_calls = n_parts = 0;
        n_handles = 0;
        while (r.p < r.end) {
            int tag = rd_u8(&r);
            if (tag == 'H') {
                uint32_t id = rd_u32(&r);
                int canonical = rd_u8(&r);
                uint64_t n = rd_u64(&r);
                const uint8_t *img = rd_bytes(&r, n);
                site_t *s = site(id);
                /* Read by dvs_handle_new: the mode the client's handle had. */
                setenv("DVS_CANONICAL", canonical ? "1" : "0", 1);
                s->h = dvs_handle_new(img, n);
                if (!s->h) {
                    fprintf(stderr, "dvs_capture_replay: handle %u: image rejected\n", id);
                    return 2;
                }
                n_handles++;
            } else if (tag == 'C') {
                replay_call(&r, n_calls++, pass == 0, warmup);
            } else if (tag == 'E') {
                uint32_t id = rd_u32(&r);
                int engine = (int)rd_u32(&r);
                if (!site(id)->h || dvs_handle_set_engine(site(id)->h, engine)) {
                    fprintf(stderr, "dvs_capture_replay: handle %u: cannot set engine %d\n", id,
                            engine);
                    return 2;
                }
            } else if (tag == 'P') {
                rd_u32(&r);
                n_parts++;
            } else {
                fprintf(stderr, "dvs_capture_replay: bad record tag 0x%02x\n", tag);
                return 2;
            }
        }
        double total = 0;
        timed_total = 0;
        for (uint32_t i = 0; i < n_sites; i++) {
            site_t *s = &sites[i];
            total += s->ns;
            timed_total += s->timed;
            if (s->timed && (pass == 0 || s->ns < s->best_ns)) s->best_ns = s->ns;
        }
        if (best_total < 0 || total < best_total) best_total = total;
    }

    printf("capture %s: %u handles, %llu calls, %llu parts calls not replayed\n", path,
           n_handles, (unsigned long long)n_calls, (unsigned long long)n_parts);
    printf("check %llu mismatches\n", (unsigned long long)n_mismatch);
    for (uint32_t i = 0; i < n_sites; i++) {
        site_t *s = &sites[i];
        if (!s->h) continue;
        if (s->timed)
            printf("handle %u: %u calls, %u timed, %.3f us/call\n", i, s->calls, s->timed,
                   s->best_ns / s->timed / 1e3);
        else
            printf("handle %u: %u calls, none timed\n", i, s->calls);
        dvs_handle_free(s->h);
    }
    if (timed_total)
        printf("total %llu timed calls, %.3f us, %.3f us/call (best of %d passes)\n",
               (unsigned long long)timed_total, best_total / 1e3,
               best_total / timed_total / 1e3, passes);
    free(sites);
    free(buf);
    return n_mismatch ? 1 : 0;
}
