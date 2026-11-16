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
 * that had none. A dvs_randomize_parts* call is counted and timed on its
 * root's handle.
 *
 * --extract ID -o OUT writes a capture of the calls rooted at handle ID, in
 * order, and of the handles they name: a standalone repro of that root.
 * Every call on ID is kept, the earlier ones giving it the state the later
 * ones see. Calls rooted elsewhere are dropped: a parts call reads only the
 * images of its other parts, the state it keeps is its root's.
 *
 *   dvs_capture_replay [--passes K] [--warmup N] [--verbose] capture.dvscap
 *   dvs_capture_replay --extract ID -o OUT capture.dvscap
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
    uint32_t calls, parts, timed;  /* parts: the calls that are parts calls */
    double ns, best_ns;
    int used;  /* --extract: a handle the calls kept name */
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
/* --extract: the root whose calls are kept. The records are then only read. */
static uint32_t extract_id;

static void mismatch(uint64_t call, uint32_t id, const char *what, uint64_t want, uint64_t got) {
    if (n_mismatch++ < 20 || verbose)
        fprintf(stderr, "mismatch: call %llu (handle %u) %s: captured 0x%llx, replayed 0x%llx\n",
                (unsigned long long)call, id, what, (unsigned long long)want,
                (unsigned long long)got);
}

static dvs_handle_t *opened(uint64_t call, uint32_t id) {
    dvs_handle_t *h = site(id)->h;
    if (!h) {
        fprintf(stderr, "dvs_capture_replay: call %llu on handle %u, not opened\n",
                (unsigned long long)call, id);
        exit(2);
    }
    return h;
}

/* A call's vars: refs to `vals`, which hold their values. */
static void rd_refs(rd_t *r, dvs_ref_t *refs, uint64_t *vals, uint32_t n_vars) {
    for (uint32_t v = 0; v < n_vars; v++) {
        int addr = rd_u8(r);
        refs[v].width = rd_u32(r);
        refs[v].is_signed = rd_u8(r);
        refs[v].layout = rd_u8(r);
        vals[v] = rd_u64(r);
        if (addr) refs[v].addr = &vals[v];  /* little-endian host */
    }
}

/* A DVS_CAP_ARRAYS input array: its size and elements. */
static void rd_array(rd_t *r, dvs_array_ref_t *a, uint8_t *eb) {
    a->n_elems = rd_u32(r);
    *eb = rd_u8(r);
    size_t n = (size_t)a->n_elems * *eb;
    if (n) {
        a->data = malloc(n);
        memcpy(a->data, rd_bytes(r, n), n);
    }
}

/* Check the vars' values after a call against the outputs; `part` names a
 * parts call's part, or is -1. */
static void check_refs(rd_t *r, uint64_t call, uint32_t id, int check, int part,
                       const dvs_ref_t *refs, const uint64_t *vals, uint32_t n_vars) {
    for (uint32_t v = 0; v < n_vars; v++) {
        size_t n = dvs_capture_ref_bytes(&refs[v]);
        if (!n) continue;
        uint64_t want = rd_u64(r);
        uint64_t got = le_value((const uint8_t *)&vals[v], n);
        if (check && want != got) {
            char what[48];
            if (part < 0) snprintf(what, sizeof what, "v%u", v);
            else snprintf(what, sizeof what, "part %d v%u", part, v);
            mismatch(call, id, what, want, got);
        }
    }
}

/* Check an array's elements after a call against the `n` elements of the
 * outputs. */
static void check_elems(rd_t *r, uint64_t call, uint32_t id, int check, const char *name,
                        const dvs_array_ref_t *a, uint8_t eb, uint32_t n) {
    const uint8_t *want = rd_bytes(r, (size_t)n * eb);
    if (!check || !eb || !n || n != a->n_elems || !memcmp(want, a->data, (size_t)n * eb)) return;
    for (uint32_t e = 0; e < n; e++) {
        uint64_t w = le_value(want + (size_t)e * eb, eb);
        uint64_t g = le_value((uint8_t *)a->data + (size_t)e * eb, eb);
        if (w != g) {
            char what[64];
            snprintf(what, sizeof what, "%s[%u]", name, e);
            mismatch(call, id, what, w, g);
            return;
        }
    }
}

static void account(site_t *s, uint64_t call, uint32_t warmup, double t) {
    s->calls++;
    if (call >= warmup) {
        s->timed++;
        s->ns += t;
    }
}

/* Replay one 'C' record (after its tag); check it when `check`. Return its
 * handle id. */
static uint32_t replay_call(rd_t *r, uint64_t call, int check, uint32_t warmup) {
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
    rd_refs(r, refs, vals, n_vars);
    for (uint32_t a = 0; a < n_arrays; a++) {
        if (kind == DVS_CAP_SIZES) sizes[a] = rd_u32(r);
        else if (kind == DVS_CAP_ARRAYS) rd_array(r, &arrays[a], &eb[a]);
    }
    int rc = 0;
    if (!extract_id) {
        dvs_handle_t *h = opened(call, id);
        double t0 = now_ns();
        rc = kind == DVS_CAP_SIZES    ? dvs_randomize_sizes(h, n_vars ? refs : NULL, sizes, seed)
             : kind == DVS_CAP_ARRAYS ? dvs_randomize_arrays(h, n_vars ? refs : NULL, arrays, modes ? any_modes : NULL, seed)
                                      : dvs_randomize(h, n_vars ? refs : NULL, modes ? any_modes : NULL, seed);
        account(site(id), call, warmup, now_ns() - t0);
    }
    int want_rc = (int)rd_u32(r);
    if (check && rc != want_rc) mismatch(call, id, "result", (uint64_t)want_rc, (uint64_t)rc);
    check_refs(r, call, id, check, -1, refs, vals, n_vars);
    for (uint32_t a = 0; a < n_arrays; a++) {
        if (kind == DVS_CAP_SIZES) {
            uint32_t want = rd_u32(r);
            if (check && want != sizes[a]) mismatch(call, id, "size", want, sizes[a]);
        } else if (kind == DVS_CAP_ARRAYS) {
            char name[32];
            snprintf(name, sizeof name, "array %u", a);
            check_elems(r, call, id, check, name, &arrays[a], eb[a], arrays[a].n_elems);
        }
        free(arrays[a].data);
    }
    free(refs);
    free(vals);
    free(sizes);
    free(arrays);
    free(eb);
    return id;
}

/* A part of a 'P' record, rebuilt. */
typedef struct {
    dvs_part_t       part;
    uint32_t         hid, n_vars, n_arrays, n_slots;
    dvs_ref_t       *refs;
    uint64_t        *vals;
    dvs_array_ref_t *arrays;
    uint8_t         *eb;
    dvs_slot_t      *slots;
} rpart_t;

static void rd_slot(rd_t *r, dvs_slot_t *sl, rpart_t *parts, uint32_t n_parts) {
    sl->state = rd_u32(r);
    uint32_t c = rd_u32(r);
    if (c != 0xFFFFFFFFu && c >= n_parts) {
        fprintf(stderr, "dvs_capture_replay: bad part index %u\n", c);
        exit(2);
    }
    sl->child = c == 0xFFFFFFFFu ? NULL : &parts[c].part;
}

/* Replay one 'P' record (after its tag) as replay_call() a 'C' record. */
static uint32_t replay_parts(rd_t *r, uint64_t call, int check, uint32_t warmup) {
    uint32_t id = rd_u32(r);
    int sizes = rd_u8(r) == DVS_CAP_SIZES;
    uint64_t seed = rd_u64(r);
    uint32_t n = rd_u32(r);
    rpart_t *ps = calloc(n ? n : 1, sizeof *ps);
    for (uint32_t i = 0; i < n; i++) {
        rpart_t *p = &ps[i];
        p->hid = rd_u32(r);
        p->n_vars = rd_u32(r);
        p->refs = calloc(p->n_vars ? p->n_vars : 1, sizeof *p->refs);
        p->vals = calloc(p->n_vars ? p->n_vars : 1, sizeof *p->vals);
        rd_refs(r, p->refs, p->vals, p->n_vars);
        p->n_arrays = rd_u32(r);
        p->arrays = calloc(p->n_arrays ? p->n_arrays : 1, sizeof *p->arrays);
        p->eb = calloc(p->n_arrays ? p->n_arrays : 1, 1);
        for (uint32_t a = 0; a < p->n_arrays; a++) rd_array(r, &p->arrays[a], &p->eb[a]);
        p->n_slots = rd_u32(r);
        p->slots = calloc(p->n_slots ? p->n_slots : 1, sizeof *p->slots);
        for (uint32_t s = 0; s < p->n_slots; s++) {
            dvs_slot_t *sl = &p->slots[s];
            if (!rd_u8(r)) {
                rd_slot(r, sl, ps, n);
                continue;
            }
            sl->n_elems = rd_u32(r);
            dvs_slot_t *el = calloc(sl->n_elems ? sl->n_elems : 1, sizeof *el);
            for (uint32_t e = 0; e < sl->n_elems; e++) rd_slot(r, &el[e], ps, n);
            sl->elems = el;
        }
        p->part = (dvs_part_t){NULL, p->refs, p->arrays, p->slots};
        if (extract_id == id) site(p->hid)->used = 1;
        else if (!extract_id && p->hid) p->part.handle = opened(call, p->hid);
    }
    int rc = 0;
    if (!extract_id) {
        double t0 = now_ns();
        rc = sizes ? dvs_randomize_parts_sizes(&ps[0].part, seed)
                   : dvs_randomize_parts(&ps[0].part, seed);
        account(site(id), call, warmup, now_ns() - t0);
        site(id)->parts++;
    }
    int want_rc = (int)rd_u32(r);
    if (check && rc != want_rc) mismatch(call, id, "result", (uint64_t)want_rc, (uint64_t)rc);
    for (uint32_t i = 0; i < n; i++) {
        rpart_t *p = &ps[i];
        check_refs(r, call, id, check, (int)i, p->refs, p->vals, p->n_vars);
        for (uint32_t a = 0; a < p->n_arrays; a++) {
            uint32_t want = rd_u32(r);
            char name[48];
            snprintf(name, sizeof name, "part %u array %u", i, a);
            if (check && want != p->arrays[a].n_elems)
                mismatch(call, id, name, want, p->arrays[a].n_elems);
            check_elems(r, call, id, check, name, &p->arrays[a], p->eb[a], want);
            free(p->arrays[a].data);
        }
        for (uint32_t s = 0; s < p->n_slots; s++) free((void *)p->slots[s].elems);
        free(p->refs);
        free(p->vals);
        free(p->arrays);
        free(p->eb);
        free(p->slots);
    }
    free(ps);
    return id;
}

/* --extract: write to `out` the calls rooted at extract_id and the handles
 * they name. The first pass finds the handles, the second writes. */
static int extract(const uint8_t *buf, const uint8_t *end, const char *out) {
    FILE *fp = fopen(out, "wb");
    if (!fp) {
        fprintf(stderr, "dvs_capture_replay: cannot write '%s'\n", out);
        return 2;
    }
    fwrite(DVSCAP_MAGIC, 1, sizeof DVSCAP_MAGIC - 1, fp);
    site(extract_id)->used = 1;
    uint32_t n_handles = 0;
    uint64_t n_calls = 0;
    for (int pass = 0; pass < 2; pass++) {
        rd_t r = {buf, end};
        while (r.p < r.end) {
            const uint8_t *rec = r.p;
            int tag = rd_u8(&r), keep;
            if (tag == 'H' || tag == 'E') {
                uint32_t id = rd_u32(&r);
                if (tag == 'H') {
                    rd_u8(&r);
                    rd_bytes(&r, rd_u64(&r));
                } else {
                    rd_u32(&r);
                }
                keep = site(id)->used;
                n_handles += pass && keep && tag == 'H';
            } else if (tag == 'C' || tag == 'P') {
                keep = (tag == 'C' ? replay_call(&r, 0, 0, 0) : replay_parts(&r, 0, 0, 0))
                       == extract_id;
                n_calls += pass && keep;
            } else {
                fprintf(stderr, "dvs_capture_replay: bad record tag 0x%02x\n", tag);
                return 2;
            }
            if (pass && keep) fwrite(rec, 1, (size_t)(r.p - rec), fp);
        }
    }
    if (fclose(fp)) {
        fprintf(stderr, "dvs_capture_replay: cannot write '%s'\n", out);
        return 2;
    }
    printf("extract handle %u: %u handles, %llu calls to %s\n", extract_id, n_handles,
           (unsigned long long)n_calls, out);
    return 0;
}

int main(int argc, char **argv) {
    int passes = 3;
    uint32_t warmup = 0;
    const char *path = NULL, *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--passes") && i + 1 < argc) passes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--extract") && i + 1 < argc) extract_id = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else {
            path = NULL;
            break;
        }
    }
    if (!path || passes < 1 || !extract_id != !out) {
        fprintf(stderr, "usage: dvs_capture_replay [--passes K] [--warmup N] [--verbose] FILE\n"
                    "       dvs_capture_replay --extract ID -o OUT FILE\n");
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
    if (extract_id) return extract(buf + ml, buf + size, out);

    uint64_t n_calls = 0, n_parts = 0;
    uint32_t n_handles = 0;
    double best_total = -1;
    uint64_t timed_total = 0;
    for (int pass = 0; pass < passes; pass++) {
        for (uint32_t i = 0; i < n_sites; i++) {
            dvs_handle_free(sites[i].h);
            sites[i].h = NULL;
            sites[i].calls = sites[i].parts = sites[i].timed = 0;
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
                replay_parts(&r, n_calls++, pass == 0, warmup);
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

    printf("capture %s: %u handles, %llu calls (%llu parts calls)\n", path,
           n_handles, (unsigned long long)n_calls, (unsigned long long)n_parts);
    printf("check %llu mismatches\n", (unsigned long long)n_mismatch);
    for (uint32_t i = 0; i < n_sites; i++) {
        site_t *s = &sites[i];
        if (!s->h) continue;
        if (s->timed)
            printf("handle %u: %u calls (%u parts), %u timed, %.3f us/call, %.3f ms\n", i,
                   s->calls, s->parts, s->timed, s->best_ns / s->timed / 1e3, s->best_ns / 1e6);
        else
            printf("handle %u: %u calls (%u parts), none timed\n", i, s->calls, s->parts);
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
