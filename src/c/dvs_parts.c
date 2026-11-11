/**
 * dvs_parts.c -- dvs_randomize_parts(): the parts of several objects solved
 * together (IEEE 1800-2023 18.5.8; docs/verilator_problem_buffer.md, "Parts").
 *
 * Each call walks the objects reached from the root through active slots,
 * depth first with the slots in order (the elements of a slot array in
 * index order, at its slot's place); an object reached again is not
 * walked again, which also ends cycles. The walk's shape -- each part's
 * class (its handle) and each slot's state, with the walk index of an
 * active slot's object, and each slot array's size -- keys the problems
 * assembled on the root's handle, the last few kept.
 *
 * Assembly declares the variables of every part anew. An import whose slot
 * path is active all the way becomes the variable of the export it names;
 * any other import is a STATE variable, read on each call through the slots
 * of that call (0 through a null slot). The constraints are rebuilt by
 * dvs_tmpl_assemble(), the parts in the order of their soft-constraint rank.
 * A foreach over a slot array is instantiated per element, and an import
 * through the element at its loop index is mapped for each element.
 */
#include <stdlib.h>
#include <string.h>
#include "dv_solve.h"
#include "dvs_problem.h"
#include "dvs_stackinfo.h"
#include "dvs_tmpl.h"
#include "dvs_capture.h"

/** Most objects one call solves. */
#define PARTS_MAX  4096u
/** Most slot-array elements one call walks. */
#define ELEMS_MAX  65536u
/** Assembled problems kept per root handle. */
#define PARTS_LRU  4u
#define NONE       0xFFFFFFFFu

/* Where a variable or an array of an assembled problem comes from. */
typedef struct {
    uint32_t   part;   /* the walk index of its part */
    uint32_t   id;     /* its variable or array id in the part */
    dvs_expr_t imp;    /* a STATE import: its ImportSpec in the part; else EXPR_NULL */
    uint32_t   elem;   /* a STATE import: the element its DVS_PATH_LOOP selects */
    uint8_t    kind;   /* its dvs_var_kind_t in the assembled problem; for an
                        * array, whether it is a slot array */
} src_t;

typedef struct {
    uint64_t     *key;
    uint32_t      n_key;
    dvs_handle_t *h;       /* NULL: an empty entry */
    src_t        *var, *arr;
    uint32_t      n_vars, n_arrays;
    uint64_t      used;    /* LRU stamp */
} shape_t;

struct dvs_shapes_s {
    shape_t  lru[PARTS_LRU];
    uint64_t stamp;
    uint32_t n_assembled;
};

/* One object of a walk. */
typedef struct {
    dvs_part_t *part;
    uint32_t    parent;     /* the walk index of the part it was reached from */
    uint32_t    next_slot;  /* the next slot to follow */
    uint32_t    next_elem;  /* in a slot array: the next element */
    uint32_t    post;       /* its place in the post-order walk; NONE while walked */
} wpart_t;

typedef struct {
    wpart_t  *p;
    uint32_t  n, cap, n_post, n_elems;
    int       sizes;   /* the size phase: no foreach over a slot array */
    uint64_t *key;
    uint32_t  n_key, cap_key;
} walk_t;

static int key_push(walk_t *w, uint64_t v) {
    if (w->n_key == w->cap_key) {
        uint32_t cap = w->cap_key ? 2 * w->cap_key : 64;
        uint64_t *k = realloc(w->key, cap * sizeof *k);
        if (!k) return DVS_RANDOMIZE_ERROR;
        w->key = k;
        w->cap_key = cap;
    }
    w->key[w->n_key++] = v;
    return 0;
}

static uint32_t find(const walk_t *w, const dvs_part_t *pt) {
    for (uint32_t i = 0; i < w->n; i++)
        if (w->p[i].part == pt) return i;
    return NONE;
}

static int walk_add(walk_t *w, dvs_part_t *pt, uint32_t parent) {
    if (!pt || !pt->handle) return DVS_RANDOMIZE_ERROR;
    if (w->n == PARTS_MAX) return DVS_RANDOMIZE_TOO_LARGE;
    if (w->n == w->cap) {
        uint32_t cap = w->cap ? 2 * w->cap : 16;
        wpart_t *p = realloc(w->p, cap * sizeof *p);
        if (!p) return DVS_RANDOMIZE_ERROR;
        w->p = p;
        w->cap = cap;
    }
    w->p[w->n++] = (wpart_t){pt, parent, 0, 0, NONE};
    return key_push(w, (uint64_t)(uintptr_t)pt->handle);
}

static const dvs_problem_t *image_of(const dvs_part_t *pt) {
    return dvs_handle_problem(pt->handle);
}

/* The array of slot `s` of image `p` if it is a slot array, else NONE */
static uint32_t slot_array(const dvs_problem_t *p, uint32_t s) {
    for (dvs_expr_t r = p->n_arrays ? p->arrays_head : EXPR_NULL; r != EXPR_NULL;) {
        const ArraySpec *a = POOL_PTR(p, r);
        if (a->slot == s) return a->array_id;
        r = a->next;
    }
    return NONE;
}

uint32_t dvs_handle_array_slot(const dvs_handle_t *h, uint32_t array_id) {
    const dvs_problem_t *p = dvs_handle_problem(h);
    for (dvs_expr_t r = p->n_arrays ? p->arrays_head : EXPR_NULL; r != EXPR_NULL;) {
        const ArraySpec *a = POOL_PTR(p, r);
        if (a->array_id == array_id) return a->slot;
        r = a->next;
    }
    return DVS_NO_SLOT;
}

/* Element `e` of slot array `a` at slot `s` of `pt`: a null slot past the
 * handles it holds */
static const dvs_slot_t *elem_slot(const dvs_part_t *pt, uint32_t s, uint32_t a, uint32_t e) {
    static const dvs_slot_t null_slot = {DVS_SLOT_NULL, 0, NULL, NULL};
    const dvs_slot_t *sl = &pt->slots[s];
    return e < pt->arrays[a].n_elems && e < sl->n_elems ? &sl->elems[e] : &null_slot;
}

/* Walk the objects reached from `root`, building the key. The root ends the
 * post-order walk; an object reached again after its walk takes a later
 * place, its highest rank. */
static int walk(walk_t *w, dvs_part_t *root) {
    int rc = walk_add(w, root, NONE);
    for (uint32_t i = 0; !rc && i != NONE;) {
        const dvs_part_t *pt = w->p[i].part;
        const uint32_t s = w->p[i].next_slot;
        if (s == dvs_handle_n_slots(pt->handle)) {
            w->p[i].post = w->n_post++;
            i = w->p[i].parent;
            continue;
        }
        const uint32_t a = slot_array(image_of(pt), s);
        const dvs_slot_t *sl;
        if (a == NONE) {
            sl = &pt->slots[w->p[i].next_slot++];
        } else if (w->p[i].next_elem == pt->arrays[a].n_elems) {
            /* The slot array's size ends its elements in the key */
            w->p[i].next_slot++;
            w->p[i].next_elem = 0;
            /* The phase too: the size phase has no foreach over a slot array */
            rc = key_push(w, (uint64_t)pt->arrays[a].n_elems << 3 | (uint64_t)w->sizes << 2 | 3);
            continue;
        } else if (++w->n_elems > ELEMS_MAX) {
            return DVS_RANDOMIZE_TOO_LARGE;
        } else {
            sl = elem_slot(pt, s, a, w->p[i].next_elem++);
        }
        if (sl->state > DVS_SLOT_ACTIVE) return DVS_RANDOMIZE_ERROR;
        if (sl->state != DVS_SLOT_ACTIVE) {
            rc = key_push(w, sl->state);
            continue;
        }
        uint32_t j = find(w, sl->child);
        if (j != NONE && w->p[j].post != NONE) w->p[j].post = w->n_post++;
        rc = key_push(w, DVS_SLOT_ACTIVE | (uint64_t)(j == NONE ? w->n : j) << 2);
        if (!rc && j == NONE) {
            rc = walk_add(w, sl->child, i);
            i = w->n - 1;
        }
    }
    return rc;
}

/* Follow the slot path of import `im` from part `pt`, `elem` the element a
 * DVS_PATH_LOOP selects: *at is the object at its end, or NULL through a null
 * slot; *active is whether every slot on the way is active. -1 for a slot
 * the object's part does not have. */
static int follow(const dvs_part_t *pt, const ImportSpec *im, uint32_t elem,
                  const dvs_part_t **at, int *active) {
    const uint32_t *path = (const uint32_t *)(im + 1);
    *active = 1;
    for (uint32_t i = 0; i < im->n_path; i++) {
        if (path[i] >= dvs_handle_n_slots(pt->handle)) return -1;
        const uint32_t a = slot_array(image_of(pt), path[i]);
        if (a != NONE && i + 1 == im->n_path) return -1;
        const dvs_slot_t *sl = a == NONE ? &pt->slots[path[i]]
                               : elem_slot(pt, path[i], a,
                                           path[i + 1] & DVS_PATH_LOOP(0) ? elem : path[i + 1]);
        i += a != NONE;
        if (sl->state == DVS_SLOT_NULL) {
            *at = NULL;
            return 0;
        }
        *active &= sl->state == DVS_SLOT_ACTIVE;
        pt = sl->child;
        if (!pt || !pt->handle) return -1;
    }
    *at = pt;
    return 0;
}

static void shape_clear(shape_t *sh) {
    dvs_handle_free(sh->h);
    free(sh->key);
    free(sh->var);
    free(sh->arr);
    memset(sh, 0, sizeof *sh);
}

void dvs_shapes_free(dvs_shapes_t *s) {
    for (uint32_t i = 0; s && i < PARTS_LRU; i++) shape_clear(&s->lru[i]);
    free(s);
}

uint32_t dvs_parts_n_assembled(dvs_handle_t *h) {
    return *dvs_handle_shapes(h) ? (*dvs_handle_shapes(h))->n_assembled : 0;
}

/* An assembly in progress, and the part it is adding (the context of the
 * dvs_asm_slots_t callbacks). */
typedef struct {
    const walk_t   *w;
    const uint32_t *moff, *map;
    const VarSpec **spec;
    dvs_builder_t  *b;
    src_t          *var;
    uint32_t        nv, cap, i;
} asm_t;

/* Declare variable `j` of the assembled problem, as `vs` */
static int declare(dvs_builder_t *b, uint32_t j, const VarSpec *vs, uint8_t kind) {
    return dvs_builder_add_var(b, j, vs->width, vs->is_signed, vs->lo, vs->hi) == EXPR_NULL
           || dvs_builder_set_var_kind(b, j, (dvs_var_kind_t)kind)
           || dvs_builder_set_var_order(b, j, vs->order);
}

/* The variable of import `r` of part `c->i` through element `elem`: the
 * export's if active all the way, else a new STATE variable; NONE on error */
static uint32_t import_var(asm_t *c, dvs_expr_t r, uint32_t elem) {
    const dvs_part_t *pt = c->w->p[c->i].part, *at;
    const ImportSpec *im = POOL_PTR(image_of(pt), r);
    int active;
    if (follow(pt, im, elem, &at, &active)) return NONE;
    if (at && active) {
        /* Every object through active slots is in the walk. */
        uint32_t ev = dvs_handle_export(at->handle, im->export_index);
        return ev == NO_EXPORT ? NONE : c->map[c->moff[find(c->w, at)] + ev];
    }
    if (c->nv == c->cap) {
        src_t *v = realloc(c->var, (c->cap = 2 * c->cap + 16) * sizeof *v);
        if (!v) return NONE;
        c->var = v;
    }
    if (declare(c->b, c->nv, c->spec[c->moff[c->i] + im->var_id], DVS_VAR_STATE)) return NONE;
    c->var[c->nv] = (src_t){c->i, im->var_id, r, elem, DVS_VAR_STATE};
    return c->nv++;
}

/* The elements a foreach over slot array `a` is instantiated for: none in
 * the size phase, which has no foreach body (IEEE 1800-2023 18.5.8.1) */
static uint32_t asm_length(void *ctx, uint32_t a) {
    const asm_t *c = ctx;
    return c->w->sizes ? 0 : c->w->p[c->i].part->arrays[a].n_elems;
}

static int asm_live(void *ctx, uint32_t a, uint32_t e) {
    const asm_t *c = ctx;
    const dvs_part_t *pt = c->w->p[c->i].part;
    return elem_slot(pt, dvs_handle_array_slot(pt->handle, a), a, e)->state != DVS_SLOT_NULL;
}

static uint32_t asm_var(void *ctx, uint32_t v, const int32_t *loop, uint32_t bound) {
    asm_t *c = ctx;
    const dvs_problem_t *p = image_of(c->w->p[c->i].part);
    for (dvs_expr_t r = p->imports_head; r != EXPR_NULL;) {
        const ImportSpec *im = POOL_PTR(p, r);
        const uint32_t *path = (const uint32_t *)(im + 1);
        if (im->var_id == v)
            for (uint32_t k = 0; k < im->n_path; k++)
                if (path[k] & DVS_PATH_LOOP(0)) {
                    uint32_t l = path[k] & ~DVS_PATH_LOOP(0);
                    return (bound >> l) & 1 ? import_var(c, r, (uint32_t)loop[l]) : NONE;
                }
        r = im->next;
    }
    return NONE;
}

/* Whether import `im` selects an element by a loop index */
static int per_elem(const ImportSpec *im) {
    for (uint32_t k = 0; k < im->n_path; k++)
        if (((const uint32_t *)(im + 1))[k] & DVS_PATH_LOOP(0)) return 1;
    return 0;
}

/* Assemble the problem of the walk `w` into `sh`; a DVS_RANDOMIZE_* code. */
static int assemble(shape_t *sh, const walk_t *w) {
    const uint32_t n = w->n;
    uint32_t *moff = malloc((n + 1) * sizeof *moff), *aoff = malloc((n + 1) * sizeof *aoff);
    uint32_t *map = NULL, *order = NULL;
    src_t *arr = NULL;
    asm_t c = {w, NULL, NULL, NULL, NULL, NULL, 0, 0, 0};
    const dvs_asm_slots_t sa = {&c, asm_length, asm_live, asm_var};
    int rc = DVS_RANDOMIZE_ERROR;
    if (!moff || !aoff) goto out;
    /* Each part's variable ids, in a map of its own, and its arrays. */
    moff[0] = aoff[0] = 0;
    for (uint32_t i = 0; i < n; i++) {
        moff[i + 1] = moff[i] + image_of(w->p[i].part)->n_vars;
        aoff[i + 1] = aoff[i] + image_of(w->p[i].part)->n_arrays;
    }
    map = malloc((moff[n] ? moff[n] : 1) * sizeof *map);
    c.spec = calloc(moff[n] ? moff[n] : 1, sizeof *c.spec);
    c.var = malloc((c.cap = moff[n] ? moff[n] : 1) * sizeof *c.var);
    arr = malloc((aoff[n] ? aoff[n] : 1) * sizeof *arr);
    order = malloc((w->n_post ? w->n_post : 1) * sizeof *order);
    c.b = dvs_builder_create(0, NULL);
    c.moff = moff;
    c.map = map;
    if (!map || !c.spec || !c.var || !arr || !order || !c.b) goto out;
    memset(map, 0xFF, (moff[n] ? moff[n] : 1) * sizeof *map);

    /* The variables of the parts, then the imports. */
    for (uint32_t i = 0; i < n; i++) {
        const dvs_part_t *pt = w->p[i].part;
        const dvs_problem_t *p = image_of(pt);
        for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
            const VarSpec *vs = POOL_PTR(p, r);
            c.spec[moff[i] + vs->var_id] = vs;
            if (vs->kind != DVS_VAR_IMPORT) {
                map[moff[i] + vs->var_id] = c.nv;
                c.var[c.nv++] = (src_t){i, vs->var_id, EXPR_NULL, NONE, vs->kind};
            }
            r = vs->next;
        }
        for (uint32_t a = 0; a < p->n_arrays; a++)
            arr[aoff[i] + a] = (src_t){i, a, EXPR_NULL, NONE,
                                       dvs_handle_array_slot(pt->handle, a) != DVS_NO_SLOT};
    }
    for (uint32_t j = 0; j < c.nv; j++)
        if (declare(c.b, j, c.spec[moff[c.var[j].part] + c.var[j].id], c.var[j].kind)) goto out;
    for (c.i = 0; c.i < n; c.i++) {
        const dvs_problem_t *p = image_of(w->p[c.i].part);
        for (dvs_expr_t r = p->n_imports ? p->imports_head : EXPR_NULL; r != EXPR_NULL;) {
            const ImportSpec *im = POOL_PTR(p, r);
            uint32_t *m = &map[moff[c.i] + im->var_id];
            if ((*m = per_elem(im) ? ASM_PER_ELEM : import_var(&c, r, NONE)) == NONE) goto out;
            r = im->next;
        }
    }

    /* The parts by rank, highest first: soft priorities follow on. */
    memset(order, 0xFF, (w->n_post ? w->n_post : 1) * sizeof *order);
    for (uint32_t i = 0; i < n; i++) order[w->p[i].post] = i;
    uint32_t prio = 0;
    for (uint32_t k = w->n_post; k-- > 0;) {
        uint32_t np;
        if ((c.i = order[k]) == NONE) continue;
        if (dvs_tmpl_assemble(c.b, image_of(w->p[c.i].part), map + moff[c.i], aoff[c.i], prio,
                              &np, &sa))
            goto out;
        prio += np;
    }
    size_t sz = 0;
    dvs_problem_t *img = dvs_builder_finalize(c.b, &sz);
    if (!img) goto out;
    sh->h = dvs_handle_new(img, sz);
    dvs_builder_free_problem(c.b, img, sz);
    if (!sh->h) goto out;
    sh->var = c.var;
    sh->arr = arr;
    sh->n_vars = c.nv;
    sh->n_arrays = aoff[n];
    c.var = arr = NULL;
    rc = DVS_RANDOMIZE_OK;
out:
    dvs_builder_destroy(c.b);
    free(moff);
    free(aoff);
    free(map);
    free(c.spec);
    free(c.var);
    free(arr);
    free(order);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Reads through a null slot                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const dvs_problem_t *p;
    const src_t         *var;
    const dvs_ref_t     *refs;
    const uint8_t       *nul;   /* per variable: an import through a null slot */
    dvs_stack_guard_t    g;
} nchk_t;

/* The truth of `r` from constants and the STATE values of the call alone:
 * 1 or 0, or -1 if anything else decides it. */
static int known(nchk_t *c, dvs_expr_t r) {
    if (dvs_stack_low(&c->g)) return -1;
    const void *n = POOL_PTR(c->p, r);
    switch (*(const uint32_t *)n) {
    case EXPR_CONST: return ((const ExprConst *)n)->value != 0;
    case EXPR_VAR: {
        uint32_t v = ((const ExprVar *)n)->var_id;
        const dvs_ref_t *rf = &c->refs[v];
        if (c->var[v].kind != DVS_VAR_STATE || c->nul[v] || !rf->addr || !rf->width
                || rf->width > 64)
            return -1;
        uint64_t u = 0;   /* little-endian, as the image */
        memcpy(&u, rf->addr, rf->width <= 8 ? 1 : rf->width <= 16 ? 2 : rf->width <= 32 ? 4 : 8);
        return (rf->width < 64 ? u & ((1ull << rf->width) - 1) : u) != 0;
    }
    case EXPR_UNARY: {
        const ExprUnary *u = n;
        int k = u->op == DVS_UN_NOT ? known(c, u->operand) : -1;
        return k < 0 ? -1 : !k;
    }
    case EXPR_BINARY: {
        const ExprBinary *bn = n;
        if (bn->op != DVS_BIN_AND && bn->op != DVS_BIN_OR) return -1;
        const int stop = bn->op == DVS_BIN_OR, a = known(c, bn->lhs), b = known(c, bn->rhs);
        return a == stop || b == stop ? stop : a == !stop && b == !stop ? !stop : -1;
    }
    default: return -1;
    }
}

/* Whether `r` reads an import through a null slot where the known values
 * do not rule the read out. */
static int live(nchk_t *c, dvs_expr_t r) {
    if (dvs_stack_low(&c->g)) return 1;
    const void *n = POOL_PTR(c->p, r);
    switch (*(const uint32_t *)n) {
    case EXPR_VAR: return c->nul[((const ExprVar *)n)->var_id];
    case EXPR_ITE: {
        const ExprITE *it = n;
        int k = known(c, it->cond);
        if (k >= 0) return live(c, k ? it->then_e : it->else_e);
        break;
    }
    case EXPR_BINARY: {
        const ExprBinary *bn = n;
        const int stop = bn->op == DVS_BIN_OR;
        if ((bn->op == DVS_BIN_AND || bn->op == DVS_BIN_OR)
                && (known(c, bn->lhs) == stop || known(c, bn->rhs) == stop))
            return 0;
        break;
    }
    default: break;
    }
    image_node_t nd;
    dvs_image_node(n, UINT64_MAX, &nd);
    for (uint64_t i = 0; i < nd.n_kids + nd.n_more; i++)
        if (live(c, i < nd.n_kids ? nd.kids[i] : nd.more[i - nd.n_kids])) return 1;
    return 0;
}

/* Whether a constraint or soft constraint of the list reads through null.
 * Their `root` is the second word of both specs. */
static int list_live(nchk_t *c, dvs_expr_t head, uint32_t count) {
    for (dvs_expr_t r = count ? head : EXPR_NULL; r != EXPR_NULL;) {
        const ConstraintSpec *cs = POOL_PTR(c->p, r);
        if (live(c, cs->root)) return 1;
        r = cs->next;
    }
    return 0;
}

static int reads_null(const shape_t *sh, const dvs_ref_t *refs, const uint8_t *nul) {
    nchk_t c = {dvs_handle_problem(sh->h), sh->var, refs, nul, {0, 0}};
    const dvs_problem_t *p = c.p;
    if (list_live(&c, p->constraints_head, p->n_constraints)
            || list_live(&c, p->softs_head, p->n_softs))
        return 1;
    for (dvs_expr_t f = p->n_foreachs ? p->foreachs_head : EXPR_NULL; f != EXPR_NULL;) {
        const ForeachSpec *fe = POOL_PTR(p, f);
        if (list_live(&c, fe->constraints_head, fe->n_constraints)
                || list_live(&c, fe->softs_head, fe->n_softs))
            return 1;
        f = fe->next;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The call                                                            */
/* ------------------------------------------------------------------ */

/* Solve `h` once: the random sizes, written back through `arr`, or the
 * elements. */
static int solve(dvs_handle_t *h, const dvs_ref_t *refs, dvs_array_ref_t *const *arr,
                 uint32_t n, int sizes, uint64_t seed) {
    dvs_array_ref_t *a = malloc((n ? n : 1) * sizeof *a);
    uint32_t *sz = malloc((n ? n : 1) * sizeof *sz);
    int rc = DVS_RANDOMIZE_ERROR;
    if (a && sz) {
        for (uint32_t k = 0; k < n; k++) {
            a[k] = *arr[k];
            sz[k] = arr[k]->n_elems;
        }
        if (!sizes) {
            rc = dvs_randomize_arrays(h, refs, a, NULL, seed);
        } else if ((rc = dvs_randomize_sizes(h, refs, sz, seed)) == DVS_RANDOMIZE_OK) {
            for (uint32_t k = 0; k < n; k++) arr[k]->n_elems = sz[k];
        }
    }
    free(a);
    free(sz);
    return rc;
}

static int parts_call(dvs_part_t *root, uint64_t seed, int sizes) {
    if (!root || !root->handle) return DVS_RANDOMIZE_ERROR;
    dvs_handle_t *rh = root->handle;
    dvs_array_ref_t **arr = NULL, *copy = NULL;
    dvs_ref_t *refs = NULL;
    uint8_t *nul = NULL, *zeros = NULL;
    walk_t w;
    memset(&w, 0, sizeof w);
    w.sizes = sizes;
    int rc = DVS_RANDOMIZE_ERROR;
    if (!dvs_handle_n_slots(rh)) {
        /* No other object: the part's own problem. */
        uint32_t n = image_of(root)->n_arrays;
        if ((arr = malloc((n ? n : 1) * sizeof *arr))) {
            for (uint32_t a = 0; a < n; a++) arr[a] = &root->arrays[a];
            rc = solve(rh, root->refs, arr, n, sizes, seed);
        }
        free(arr);
        return rc;
    }
    dvs_shapes_t **sp = dvs_handle_shapes(rh);
    if ((rc = walk(&w, root)) != 0) goto out;
    rc = DVS_RANDOMIZE_ERROR;
    if (!*sp && !(*sp = calloc(1, sizeof **sp))) goto out;

    /* The problem of this shape. */
    shape_t *sh = NULL, *victim = &(*sp)->lru[0];
    for (uint32_t i = 0; i < PARTS_LRU && !sh; i++) {
        shape_t *c = &(*sp)->lru[i];
        if (c->h && c->n_key == w.n_key && !memcmp(c->key, w.key, w.n_key * sizeof *w.key))
            sh = c;
        else if (!c->h || (victim->h && c->used < victim->used))
            victim = c;
    }
    if (!sh) {
        sh = victim;
        shape_clear(sh);
        (*sp)->n_assembled++;
        if ((rc = assemble(sh, &w)) != DVS_RANDOMIZE_OK) {
            shape_clear(sh);
            goto out;
        }
        sh->key = w.key;
        sh->n_key = w.n_key;
        w.key = NULL;
    }
    sh->used = ++(*sp)->stamp;

    /* Its refs and arrays in the objects of this call. */
    rc = DVS_RANDOMIZE_ERROR;
    static const uint64_t zero = 0;
    refs = malloc((sh->n_vars ? sh->n_vars : 1) * sizeof *refs);
    arr = malloc((sh->n_arrays ? sh->n_arrays : 1) * sizeof *arr);
    nul = calloc(sh->n_vars ? sh->n_vars : 1, 1);
    if (!refs || !arr || !nul) goto out;
    int any_null = 0;
    for (uint32_t j = 0; j < sh->n_vars; j++) {
        const src_t *s = &sh->var[j];
        const dvs_part_t *pt = w.p[s->part].part, *at;
        if (s->imp == EXPR_NULL) {
            refs[j] = pt->refs[s->id];
            continue;
        }
        const ImportSpec *im = POOL_PTR(image_of(pt), s->imp);
        int active;
        if (follow(pt, im, s->elem, &at, &active)) goto out;
        if (!at) {
            refs[j] = (dvs_ref_t){(void *)&zero, 64, 0, DVS_REF_SCALAR, 0};
            nul[j] = 1;
            any_null = 1;
            continue;
        }
        uint32_t ev = dvs_handle_export(at->handle, im->export_index);
        if (ev == NO_EXPORT) goto out;
        refs[j] = at->refs[ev];
    }
    /* A slot array's elements are not read: solved in a copy reading zeros */
    uint32_t most = 0;
    for (uint32_t k = 0; k < sh->n_arrays; k++) {
        arr[k] = &w.p[sh->arr[k].part].part->arrays[sh->arr[k].id];
        if (sh->arr[k].kind && arr[k]->n_elems > most) most = arr[k]->n_elems;
    }
    if (!(copy = malloc((sh->n_arrays ? sh->n_arrays : 1) * sizeof *copy))
            || !(zeros = calloc(most + 1, 1)))
        goto out;
    for (uint32_t k = 0; k < sh->n_arrays; k++) {
        if (!sh->arr[k].kind) continue;
        copy[k] = (dvs_array_ref_t){zeros, arr[k]->n_elems, 0};
        arr[k] = &copy[k];
    }
    rc = any_null && reads_null(sh, refs, nul)
             ? DVS_RANDOMIZE_NULL
             : solve(sh->h, refs, arr, sh->n_arrays, sizes, seed);
    for (uint32_t k = 0; k < sh->n_arrays; k++)
        if (sh->arr[k].kind)
            w.p[sh->arr[k].part].part->arrays[sh->arr[k].id].n_elems = copy[k].n_elems;
out:
    free(w.p);
    free(w.key);
    free(refs);
    free(arr);
    free(copy);
    free(zeros);
    free(nul);
    return rc;
}

/* parts_call, noted in a capture (DVS_CAPTURE) when not made by another call. */
static int parts_api(dvs_part_t *root, uint64_t seed, int sizes) {
    if (dvs_capture_enter() && root && dvs_handle_cap_id(root->handle))
        dvs_capture_parts(dvs_handle_cap_id(root->handle));
    int rc = parts_call(root, seed, sizes);
    dvs_capture_leave();
    return rc;
}

int dvs_randomize_parts(dvs_part_t *root, uint64_t seed) {
    return parts_api(root, seed, 0);
}

int dvs_randomize_parts_sizes(dvs_part_t *root, uint64_t seed) {
    return parts_api(root, seed, 1);
}
