/**
 * dvs_image.c -- the problem image format: its layout, pinned; the check
 * that an image is safe to read; and dvs_problem_validate(). Needs only libc.
 *
 * Compiled into both the writer and the runtime, so a compiler or ABI that
 * would lay the image out differently fails to build either, rather than
 * write or read problems the other side misreads. The numbers below are the
 * format (DVS_PROBLEM_FORMAT_VERSION): change one and bump the version.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dvs_problem.h"
#include "dvs_sv.h"           /* DVS_SV_MAX_DEPTH */

#define SIZE(T, n)   _Static_assert(sizeof(T) == (n), "size of " #T)
#define AT(T, f, o)  _Static_assert(offsetof(T, f) == (o), "offset of " #T "." #f)

SIZE(ExprConst, 24); AT(ExprConst, kind, 0); AT(ExprConst, is_signed, 4);
AT(ExprConst, _p8, 5); AT(ExprConst, _p16, 6); AT(ExprConst, width, 8);
AT(ExprConst, _p32, 12); AT(ExprConst, value, 16);
SIZE(ExprConstWide, 16); AT(ExprConstWide, kind, 0); AT(ExprConstWide, is_signed, 4);
AT(ExprConstWide, _p8, 5); AT(ExprConstWide, width, 8); AT(ExprConstWide, n_limbs, 12);
AT(ExprConstWide, limbs, 16);
SIZE(ExprVar, 8); AT(ExprVar, kind, 0); AT(ExprVar, var_id, 4);
SIZE(ExprBinary, 16); AT(ExprBinary, kind, 0); AT(ExprBinary, op, 4);
AT(ExprBinary, lhs, 8); AT(ExprBinary, rhs, 12);
SIZE(ExprUnary, 12); AT(ExprUnary, kind, 0); AT(ExprUnary, op, 4);
AT(ExprUnary, operand, 8);
SIZE(ExprITE, 16); AT(ExprITE, kind, 0); AT(ExprITE, cond, 4); AT(ExprITE, then_e, 8);
AT(ExprITE, else_e, 12);
SIZE(ExprInRange, 16); AT(ExprInRange, kind, 0); AT(ExprInRange, value, 4);
AT(ExprInRange, lo, 8); AT(ExprInRange, hi, 12);
SIZE(ExprInSet, 12); AT(ExprInSet, kind, 0); AT(ExprInSet, value, 4);
AT(ExprInSet, n_elems, 8);
SIZE(ExprInRanges, 12); AT(ExprInRanges, kind, 0); AT(ExprInRanges, value, 4);
AT(ExprInRanges, n_ranges, 8);
SIZE(ExprExtend, 20); AT(ExprExtend, kind, 0); AT(ExprExtend, sign_extend, 4);
AT(ExprExtend, _p8, 5); AT(ExprExtend, _p16, 6); AT(ExprExtend, from_bits, 8);
AT(ExprExtend, to_bits, 12); AT(ExprExtend, operand, 16);
SIZE(ExprSvCast, 20); AT(ExprSvCast, kind, 0); AT(ExprSvCast, sign_extend, 4);
AT(ExprSvCast, dst_signed, 5); AT(ExprSvCast, _p16, 6); AT(ExprSvCast, from_bits, 8);
AT(ExprSvCast, to_bits, 12); AT(ExprSvCast, operand, 16);
SIZE(ExprCast, 16); AT(ExprCast, kind, 0); AT(ExprCast, to_bits, 4);
AT(ExprCast, to_signed, 8); AT(ExprCast, _p8, 9); AT(ExprCast, _p16, 10);
AT(ExprCast, operand, 12);
SIZE(ExprExtract, 16); AT(ExprExtract, kind, 0); AT(ExprExtract, hi_bit, 4);
AT(ExprExtract, lo_bit, 8); AT(ExprExtract, operand, 12);
SIZE(ExprConcat, 16); AT(ExprConcat, kind, 0); AT(ExprConcat, lo_width, 4);
AT(ExprConcat, hi, 8); AT(ExprConcat, lo, 12);
SIZE(ExprSum, 12); AT(ExprSum, kind, 0); AT(ExprSum, result, 4);
AT(ExprSum, n_vars, 8);
SIZE(ExprCountones, 12); AT(ExprCountones, kind, 0); AT(ExprCountones, result, 4);
AT(ExprCountones, operand, 8);
SIZE(ExprClog2, 12); AT(ExprClog2, kind, 0); AT(ExprClog2, result, 4);
AT(ExprClog2, operand, 8);
SIZE(ExprArraySelect, 20); AT(ExprArraySelect, kind, 0);
AT(ExprArraySelect, base_var_id, 4); AT(ExprArraySelect, n_elems, 8);
AT(ExprArraySelect, result, 12); AT(ExprArraySelect, index, 16);
SIZE(VarSpec, 32); AT(VarSpec, next, 0); AT(VarSpec, var_id, 4); AT(VarSpec, width, 8);
AT(VarSpec, is_signed, 12); AT(VarSpec, kind, 13); AT(VarSpec, order, 14);
AT(VarSpec, _p8, 15); AT(VarSpec, lo, 16); AT(VarSpec, hi, 24);
SIZE(ConstraintSpec, 12); AT(ConstraintSpec, next, 0); AT(ConstraintSpec, root, 4);
AT(ConstraintSpec, constraint_id, 8);
SIZE(SourceSpec, 8); AT(SourceSpec, next, 0); AT(SourceSpec, n_vars, 4);
SIZE(AllDiffSpec, 8); AT(AllDiffSpec, next, 0); AT(AllDiffSpec, n_vars, 4);
SIZE(SoftSpec, 16); AT(SoftSpec, next, 0); AT(SoftSpec, root, 4);
AT(SoftSpec, priority, 8); AT(SoftSpec, constraint_id, 12);
SIZE(DistSpec, 16); AT(DistSpec, next, 0); AT(DistSpec, var_id, 4);
AT(DistSpec, n_entries, 8); AT(DistSpec, index, 12);
SIZE(ExprArraySize, 8); AT(ExprArraySize, kind, 0); AT(ExprArraySize, array_id, 4);
SIZE(ExprLoopIndex, 8); AT(ExprLoopIndex, kind, 0); AT(ExprLoopIndex, level, 4);
SIZE(ExprElem, 16); AT(ExprElem, kind, 0); AT(ExprElem, array_id, 4);
AT(ExprElem, index, 8); AT(ExprElem, _pad, 12);
SIZE(ExprReduce, 20); AT(ExprReduce, kind, 0); AT(ExprReduce, op, 4);
AT(ExprReduce, array_id, 8); AT(ExprReduce, level, 12); AT(ExprReduce, body, 16);
SIZE(ArraySpec, 24); AT(ArraySpec, next, 0); AT(ArraySpec, array_id, 4);
AT(ArraySpec, elem_width, 8); AT(ArraySpec, elem_signed, 12); AT(ArraySpec, kind, 13);
AT(ArraySpec, order, 14); AT(ArraySpec, size_random, 15); AT(ArraySpec, slot, 16);
AT(ArraySpec, _p32, 20);
SIZE(ForeachSpec, 40); AT(ForeachSpec, next, 0); AT(ForeachSpec, parent, 4);
AT(ForeachSpec, array_id, 8); AT(ForeachSpec, level, 12);
AT(ForeachSpec, constraints_head, 16); AT(ForeachSpec, n_constraints, 20);
AT(ForeachSpec, softs_head, 24); AT(ForeachSpec, n_softs, 28);
AT(ForeachSpec, dists_head, 32); AT(ForeachSpec, n_dists, 36);
SIZE(dvs_dist_entry_t, 24); AT(dvs_dist_entry_t, lo, 0); AT(dvs_dist_entry_t, hi, 8);
AT(dvs_dist_entry_t, weight, 16); AT(dvs_dist_entry_t, is_per_value, 20);
AT(dvs_dist_entry_t, _dpad, 21);
SIZE(dvs_pool_t, 16); AT(dvs_pool_t, capacity, 0); AT(dvs_pool_t, used, 4);
AT(dvs_pool_t, overflow, 8); AT(dvs_pool_t, _pad, 12);
SIZE(ExportSpec, 8); AT(ExportSpec, next, 0); AT(ExportSpec, var_id, 4);
SIZE(ImportSpec, 16); AT(ImportSpec, next, 0); AT(ImportSpec, var_id, 4);
AT(ImportSpec, export_index, 8); AT(ImportSpec, n_path, 12);
SIZE(dvs_problem_t, 120); AT(dvs_problem_t, magic, 0);
AT(dvs_problem_t, format_version, 4); AT(dvs_problem_t, n_vars, 8);
AT(dvs_problem_t, n_constraints, 12); AT(dvs_problem_t, n_sources, 16);
AT(dvs_problem_t, vars_head, 20); AT(dvs_problem_t, constraints_head, 24);
AT(dvs_problem_t, sources_head, 28); AT(dvs_problem_t, n_alldiffs, 32);
AT(dvs_problem_t, allDiff_head, 36); AT(dvs_problem_t, n_softs, 40);
AT(dvs_problem_t, softs_head, 44); AT(dvs_problem_t, n_dists, 48);
AT(dvs_problem_t, dists_head, 52); AT(dvs_problem_t, next_constraint_id, 56);
AT(dvs_problem_t, flags, 60); AT(dvs_problem_t, n_arrays, 64);
AT(dvs_problem_t, arrays_head, 68); AT(dvs_problem_t, n_foreachs, 72);
AT(dvs_problem_t, foreachs_head, 76); AT(dvs_problem_t, n_slots, 80);
AT(dvs_problem_t, n_exports, 84); AT(dvs_problem_t, exports_head, 88);
AT(dvs_problem_t, n_imports, 92); AT(dvs_problem_t, imports_head, 96);
AT(dvs_problem_t, _p32, 100); AT(dvs_problem_t, pool, 104);

/* The stored enumerations: the last value of each. */
_Static_assert(EXPR_REDUCE == 21 && DVS_BIN_ASHR == 18 && DVS_UN_INVERT == 2
               && DVS_VAR_IMPORT == 4, "stored enumeration values");

/* The fixed part of each node kind, by ExprKind. */
static const uint8_t _node_fixed[] = {
    sizeof(ExprConst), sizeof(ExprVar), sizeof(ExprBinary), sizeof(ExprUnary),
    sizeof(ExprITE), sizeof(ExprInRange), sizeof(ExprInSet), sizeof(ExprExtend),
    sizeof(ExprExtract), sizeof(ExprConcat), sizeof(ExprSum), sizeof(ExprCountones),
    sizeof(ExprClog2), sizeof(ExprArraySelect), sizeof(ExprInRanges), sizeof(ExprSvCast),
    sizeof(ExprCast), sizeof(ExprConstWide), sizeof(ExprArraySize), sizeof(ExprLoopIndex),
    sizeof(ExprElem), sizeof(ExprReduce),
};
_Static_assert(sizeof _node_fixed == EXPR_REDUCE + 1, "a size for every kind");

int dvs_image_node(const void *node, uint64_t avail, image_node_t *out) {
    memset(out, 0, sizeof *out);
    if (avail < sizeof(uint32_t)) return -1;
    uint32_t kind = *(const uint32_t *)node;
    if (kind >= sizeof _node_fixed || avail < _node_fixed[kind]) return -1;
    out->size = _node_fixed[kind];
    dvs_expr_t *k = out->kids;
    switch (kind) {
    case EXPR_CONST_WIDE: {
        const ExprConstWide *n = node;
        if (n->width == 0 || n->n_limbs != ((uint64_t)n->width + 63) / 64) return -1;
        out->size += (uint64_t)n->n_limbs * sizeof(uint64_t);
        break;
    }
    case EXPR_BINARY: {
        const ExprBinary *n = node;
        if (n->op > DVS_BIN_ASHR) return -1;
        k[0] = n->lhs; k[1] = n->rhs; out->n_kids = 2;
        break;
    }
    case EXPR_UNARY: {
        const ExprUnary *n = node;
        if (n->op > DVS_UN_INVERT) return -1;
        k[0] = n->operand; out->n_kids = 1;
        break;
    }
    case EXPR_ITE: {
        const ExprITE *n = node;
        k[0] = n->cond; k[1] = n->then_e; k[2] = n->else_e; out->n_kids = 3;
        break;
    }
    case EXPR_IN_RANGE: {
        const ExprInRange *n = node;
        k[0] = n->value; k[1] = n->lo; k[2] = n->hi; out->n_kids = 3;
        break;
    }
    case EXPR_IN_SET: {
        const ExprInSet *n = node;
        k[0] = n->value; out->n_kids = 1;
        out->more = (const dvs_expr_t *)(n + 1); out->n_more = n->n_elems;
        break;
    }
    case EXPR_IN_RANGES: {
        const ExprInRanges *n = node;
        k[0] = n->value; out->n_kids = 1;
        out->more = (const dvs_expr_t *)(n + 1);
        out->n_more = (uint64_t)n->n_ranges * 2;
        break;
    }
    case EXPR_SUM: {
        const ExprSum *n = node;
        k[0] = n->result; out->n_kids = 1;
        out->more = (const dvs_expr_t *)(n + 1); out->n_more = n->n_vars;
        break;
    }
    case EXPR_EXTEND:    k[0] = ((const ExprExtend *)node)->operand; out->n_kids = 1; break;
    case EXPR_SV_CAST:   k[0] = ((const ExprSvCast *)node)->operand; out->n_kids = 1; break;
    case EXPR_CAST:      k[0] = ((const ExprCast *)node)->operand; out->n_kids = 1; break;
    case EXPR_EXTRACT:   k[0] = ((const ExprExtract *)node)->operand; out->n_kids = 1; break;
    case EXPR_CONCAT: {
        const ExprConcat *n = node;
        k[0] = n->hi; k[1] = n->lo; out->n_kids = 2;
        break;
    }
    case EXPR_COUNTONES: {
        const ExprCountones *n = node;
        k[0] = n->result; k[1] = n->operand; out->n_kids = 2;
        break;
    }
    case EXPR_CLOG2: {
        const ExprClog2 *n = node;
        k[0] = n->result; k[1] = n->operand; out->n_kids = 2;
        break;
    }
    case EXPR_ARRAY_SELECT: {
        const ExprArraySelect *n = node;
        k[0] = n->result; k[1] = n->index; out->n_kids = 2;
        break;
    }
    case EXPR_ELEM:   k[0] = ((const ExprElem *)node)->index; out->n_kids = 1; break;
    case EXPR_REDUCE: {
        const ExprReduce *n = node;
        if (n->op != DVS_BIN_ADD && n->op != DVS_BIN_MUL && n->op != DVS_BIN_BAND
                && n->op != DVS_BIN_BOR && n->op != DVS_BIN_BXOR)
            return -1;
        k[0] = n->body; out->n_kids = 1;
        break;
    }
    default: break;   /* EXPR_CONST, EXPR_VAR, EXPR_ARRAY_SIZE, EXPR_LOOP_INDEX: leaves */
    }
    out->size += out->n_more * sizeof(dvs_expr_t);
    return out->size <= avail ? 0 : -1;
}

/* The spec lists of the header: where each starts and counts, the size of
 * an item, and of the array that follows it (`n_at` 0: none). An item with
 * `var` set has a variable id at offset 4. */
static const struct {
    uint32_t head, count, size, n_at, elem, root_at, var;
    const char *what;
} _lists[] = {
    { offsetof(dvs_problem_t, vars_head), offsetof(dvs_problem_t, n_vars),
      sizeof(VarSpec), 0, 0, 0, 1, "variable" },
    { offsetof(dvs_problem_t, exports_head), offsetof(dvs_problem_t, n_exports),
      sizeof(ExportSpec), 0, 0, 0, 1, "export" },
    { offsetof(dvs_problem_t, imports_head), offsetof(dvs_problem_t, n_imports),
      sizeof(ImportSpec), offsetof(ImportSpec, n_path), sizeof(uint32_t), 0, 1, "import" },
    { offsetof(dvs_problem_t, constraints_head), offsetof(dvs_problem_t, n_constraints),
      sizeof(ConstraintSpec), 0, 0, offsetof(ConstraintSpec, root), 0, "constraint" },
    { offsetof(dvs_problem_t, sources_head), offsetof(dvs_problem_t, n_sources),
      sizeof(SourceSpec), offsetof(SourceSpec, n_vars), sizeof(uint32_t), 0, 0, "source" },
    { offsetof(dvs_problem_t, allDiff_head), offsetof(dvs_problem_t, n_alldiffs),
      sizeof(AllDiffSpec), offsetof(AllDiffSpec, n_vars), sizeof(uint32_t), 0, 0,
      "all-different" },
    { offsetof(dvs_problem_t, softs_head), offsetof(dvs_problem_t, n_softs),
      sizeof(SoftSpec), 0, 0, offsetof(SoftSpec, root), 0, "soft constraint" },
    { offsetof(dvs_problem_t, dists_head), offsetof(dvs_problem_t, n_dists),
      sizeof(DistSpec), offsetof(DistSpec, n_entries), sizeof(dvs_dist_entry_t), 0, 0,
      "distribution" },
};

static int _fail(char *why, size_t whylen, const char *fmt, ...) {
    if (why && whylen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, whylen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

#define U32_AT(base, off)  (*(const uint32_t *)((const uint8_t *)(base) + (off)))

int dvs_problem_check_image(const void *img, size_t size, char *why, size_t whylen) {
    const size_t pool_at = offsetof(dvs_problem_t, pool);
    if ((uintptr_t)img % DVS_IMAGE_ALIGN)
        return _fail(why, whylen, "image is not %u-byte aligned", DVS_IMAGE_ALIGN);
    if (size < sizeof(dvs_problem_t))
        return _fail(why, whylen, "image is shorter than its header");
    const dvs_problem_t *p = img;
    if (memcmp(img, "DVSP", 4) != 0)
        return _fail(why, whylen, "not a problem image (bad magic)");
    if (p->magic != DVS_PROBLEM_MAGIC)
        return _fail(why, whylen, "the image is little-endian, this host is not");
    if (p->format_version != DVS_PROBLEM_FORMAT_VERSION)
        return _fail(why, whylen, "image format version %u, this library reads %u",
                     p->format_version, DVS_PROBLEM_FORMAT_VERSION);
    /* The runtime may allocate up to `capacity` in a copy of `size` bytes. */
    if (p->pool.overflow || p->pool.used > p->pool.capacity
            || p->pool.capacity > size - pool_at - sizeof(dvs_pool_t))
        return _fail(why, whylen, "pool header does not fit the image");

    /* Offsets are from &p->pool; nodes lie in [sizeof(dvs_pool_t), end). */
    const uint8_t *pool = (const uint8_t *)img + pool_at;
    const uint64_t end = sizeof(dvs_pool_t) + (uint64_t)p->pool.used;
#define OFF_OK(r, need)  ((r) >= sizeof(dvs_pool_t) && (r) % DVS_IMAGE_ALIGN == 0 \
                          && (uint64_t)(r) + (need) <= end)
    uint32_t n_slots = (uint32_t)(((uint64_t)p->pool.used + DVS_IMAGE_ALIGN - 1)
                                  / DVS_IMAGE_ALIGN);
    uint8_t *reach = calloc(n_slots / 8 + 1, 1);
    if (!reach) return _fail(why, whylen, "out of memory");
#define SLOT(r)  (((r) - (uint32_t)sizeof(dvs_pool_t)) / DVS_IMAGE_ALIGN)
#define MARK(r)  (reach[SLOT(r) / 8] |= (uint8_t)(1u << (SLOT(r) % 8)))
    int rc = 0;

    for (size_t l = 0; l < sizeof _lists / sizeof _lists[0] && !rc; l++) {
        uint32_t count = U32_AT(p, _lists[l].count), i = 0;
        for (dvs_expr_t r = U32_AT(p, _lists[l].head); r != EXPR_NULL && !rc; i++) {
            if (i == count || !OFF_OK(r, _lists[l].size)) {
                rc = _fail(why, whylen, "%s list item %u is out of range or one too many",
                           _lists[l].what, i);
                break;
            }
            const uint8_t *s = pool + r;
            uint64_t n = _lists[l].n_at ? U32_AT(s, _lists[l].n_at) : 0;
            if ((uint64_t)r + _lists[l].size + n * _lists[l].elem > end
                    || (_lists[l].var && U32_AT(s, 4) >= p->n_vars)
                    || (l == 0 && ((const VarSpec *)s)->kind > DVS_VAR_IMPORT)
                    /* an import's first slot is one of this part's */
                    || (l == 2 && (n == 0 || U32_AT(s, sizeof(ImportSpec)) >= p->n_slots)))
                rc = _fail(why, whylen, "%s list item %u is malformed", _lists[l].what, i);
            else if (_lists[l].root_at) {
                dvs_expr_t root = U32_AT(s, _lists[l].root_at);
                if (OFF_OK(root, sizeof(uint32_t))) MARK(root);
                else rc = _fail(why, whylen, "%s %u has a bad root", _lists[l].what, i);
            }
            r = U32_AT(s, 0);   /* next */
        }
        if (!rc && i != count)
            rc = _fail(why, whylen, "%s list has %u items, the header says %u",
                       _lists[l].what, i, count);
    }

    /* Arrays, and foreachs with their bodies (a problem written before
     * arrays existed may leave these heads 0: a count of 0 skips them). */
    uint32_t ai = 0;
    for (dvs_expr_t r = p->n_arrays ? p->arrays_head : EXPR_NULL; r != EXPR_NULL && !rc;
         ai++) {
        const ArraySpec *a = (const ArraySpec *)(pool + r);
        if (ai == p->n_arrays || !OFF_OK(r, sizeof(ArraySpec)))
            rc = _fail(why, whylen, "array list item %u is out of range or one too many", ai);
        else if (a->array_id >= p->n_arrays || a->elem_width == 0 || a->elem_width > 64
                 || (a->kind != DVS_VAR_RAND && a->kind != DVS_VAR_STATE)
                 || (a->slot != DVS_NO_SLOT && a->slot >= p->n_slots))
            rc = _fail(why, whylen, "array list item %u is malformed", ai);
        else
            r = a->next;
    }
    if (!rc && p->n_arrays && ai != p->n_arrays)
        rc = _fail(why, whylen, "array list has %u items, the header says %u", ai,
                   p->n_arrays);
    ai = 0;
    for (dvs_expr_t r = p->n_foreachs ? p->foreachs_head : EXPR_NULL;
         r != EXPR_NULL && !rc; ai++) {
        const ForeachSpec *fe = (const ForeachSpec *)(pool + r);
        if (ai == p->n_foreachs || !OFF_OK(r, sizeof(ForeachSpec))) {
            rc = _fail(why, whylen, "foreach list item %u is out of range or one too many",
                       ai);
            break;
        }
        if (fe->array_id >= p->n_arrays || fe->level >= DVS_MAX_LOOP_LEVELS
                || (fe->parent != EXPR_NULL && !OFF_OK(fe->parent, sizeof(ForeachSpec)))) {
            rc = _fail(why, whylen, "foreach %u is malformed", ai);
            break;
        }
        /* The body: constraints, soft constraints, distributions. */
        const struct { dvs_expr_t head; uint32_t count, size, root_at; } body[] = {
            { fe->constraints_head, fe->n_constraints, sizeof(ConstraintSpec),
              offsetof(ConstraintSpec, root) },
            { fe->softs_head, fe->n_softs, sizeof(SoftSpec), offsetof(SoftSpec, root) },
            { fe->dists_head, fe->n_dists, sizeof(DistSpec), offsetof(DistSpec, index) },
        };
        for (size_t l = 0; l < 3 && !rc; l++) {
            uint32_t j = 0;
            for (dvs_expr_t c = body[l].count ? body[l].head : EXPR_NULL;
                 c != EXPR_NULL && !rc; j++) {
                const uint8_t *cs = pool + c;
                uint64_t extra = l == 2 ? (uint64_t)((const DistSpec *)cs)->n_entries
                                              * sizeof(dvs_dist_entry_t) : 0;
                dvs_expr_t root = 0;
                if (j == body[l].count || !OFF_OK(c, body[l].size + extra)
                        || (l == 2 && ((const DistSpec *)cs)->var_id >= p->n_arrays)
                        || !OFF_OK(root = U32_AT(cs, body[l].root_at), sizeof(uint32_t)))
                    rc = _fail(why, whylen, "the body of foreach %u is malformed", ai);
                else {
                    MARK(root);
                    c = U32_AT(cs, 0);
                }
            }
            if (!rc && body[l].count && j != body[l].count)
                rc = _fail(why, whylen, "the body of foreach %u is malformed", ai);
        }
        r = fe->next;
    }
    if (!rc && p->n_foreachs && ai != p->n_foreachs)
        rc = _fail(why, whylen, "foreach list has %u items, the header says %u", ai,
                   p->n_foreachs);

    /* Each node's children lie below it, so one pass from the top visits
     * every node reachable from a root after all of its parents. */
    for (uint32_t s = n_slots; s-- > 0 && !rc;) {
        if (!(reach[s / 8] & (1u << (s % 8)))) continue;
        dvs_expr_t r = (dvs_expr_t)(sizeof(dvs_pool_t) + (uint64_t)s * DVS_IMAGE_ALIGN);
        image_node_t nd;
        if (dvs_image_node(pool + r, end - r, &nd) != 0) {
            rc = _fail(why, whylen, "expression node at offset %u is malformed", r);
            break;
        }
        /* Array nodes name a declared array and a level below the limit. */
        const uint32_t *w = (const uint32_t *)(pool + r);
        if (((w[0] == EXPR_ARRAY_SIZE || w[0] == EXPR_ELEM) && w[1] >= p->n_arrays)
                || (w[0] == EXPR_LOOP_INDEX && w[1] >= DVS_MAX_LOOP_LEVELS)
                || (w[0] == EXPR_REDUCE && (w[2] >= p->n_arrays
                                            || w[3] >= DVS_MAX_LOOP_LEVELS))) {
            rc = _fail(why, whylen, "expression node at offset %u names a bad array or "
                       "level", r);
            break;
        }
        for (uint64_t i = 0; i < nd.n_kids + nd.n_more; i++) {
            dvs_expr_t c = i < nd.n_kids ? nd.kids[i] : nd.more[i - nd.n_kids];
            if (c >= r || !OFF_OK(c, sizeof(uint32_t))) {
                rc = _fail(why, whylen, "expression node at offset %u has a bad child", r);
                break;
            }
            MARK(c);
        }
    }
#undef OFF_OK
#undef SLOT
#undef MARK
    free(reach);
    return rc;
}

/* ------------------------------------------------------------------ */
/* dvs_problem_validate() -- refuse, when a problem is written, what   */
/* the runtime would refuse when it compiles it. It mirrors the checks */
/* dvs_solver_compile() makes inline, each named by the code compile   */
/* returns for it, so that a compiler embedding problems (Verilator)   */
/* can report them at its own compile time. tests/c/test_image.c keeps */
/* the two in step: every check has a problem that both refuse.        */
/* ------------------------------------------------------------------ */

/* SV elaboration refuses a width over DVS_MAX_WIDTH, and a reversed slice. */
static int _node_widths(const void *node, char *why, size_t whylen, uint32_t at) {
    uint32_t w = 0;
    switch (*(const uint32_t *)node) {
    case EXPR_CONST:  w = ((const ExprConst *)node)->width; break;
    case EXPR_CONST_WIDE: w = ((const ExprConstWide *)node)->width; break;
    case EXPR_EXTEND: {
        const ExprExtend *n = node;
        w = n->from_bits > n->to_bits ? n->from_bits : n->to_bits;
        break;
    }
    case EXPR_SV_CAST: {
        const ExprSvCast *n = node;
        w = n->from_bits > n->to_bits ? n->from_bits : n->to_bits;
        break;
    }
    case EXPR_CAST:   w = ((const ExprCast *)node)->to_bits; break;
    case EXPR_CONCAT: w = ((const ExprConcat *)node)->lo_width; break;
    case EXPR_EXTRACT: {
        const ExprExtract *n = node;
        if (n->hi_bit < n->lo_bit)
            return _fail(why, whylen, "UNSUPPORTED_WIDTH: slice [%u:%u] at offset %u is "
                         "reversed", n->hi_bit, n->lo_bit, at);
        w = n->hi_bit + 1;
        break;
    }
    default: break;
    }
    if (w > DVS_MAX_WIDTH)
        return _fail(why, whylen, "UNSUPPORTED_WIDTH: a width of %u bits at offset %u "
                     "is over %u", w, at, DVS_MAX_WIDTH);
    return 0;
}

int dvs_problem_validate(const dvs_problem_t *p, char *why, size_t whylen) {
    if (!p) return _fail(why, whylen, "no problem");
    if (dvs_problem_check_image(p, sizeof *p + (size_t)p->pool.capacity, why, whylen))
        return -1;
    const dvs_pool_t *pool = &p->pool;
    const uint32_t n = p->n_vars;
    int rc = 0;

    /* dvs_solver_compile: the ids are 0..n-1, each declared once, with a
     * width of 1 to 64 (a wider variable is UNSUPPORTED_WIDTH). Each var is
     * also where a dvs_ref_t (1..64 bits) reads or writes it. */
    uint32_t *width = calloc(n ? n : 1, sizeof *width);
    if (!width) return _fail(why, whylen, "out of memory");
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL && !rc;) {
        const VarSpec *vs = dvs_pool_ptr(pool, r);
        if (vs->var_id >= n)
            rc = _fail(why, whylen, "BAD_VAR: variable id %u, with %u variables",
                       vs->var_id, n);
        else if (width[vs->var_id])
            rc = _fail(why, whylen, "BAD_VAR: variable %u is declared twice", vs->var_id);
        else if (vs->width == 0)
            rc = _fail(why, whylen, "BAD_VAR: variable %u has width 0", vs->var_id);
        else if (vs->width > 64)
            rc = _fail(why, whylen, "UNSUPPORTED_WIDTH: variable %u is %u bits, over 64",
                       vs->var_id, vs->width);
        else
            width[vs->var_id] = vs->width;
        r = vs->next;
    }

    /* The element widths of the arrays (a member of an all-different). */
    uint32_t *awidth = calloc(p->n_arrays ? p->n_arrays : 1, sizeof *awidth);
    if (!awidth) { free(width); return _fail(why, whylen, "out of memory"); }
    for (dvs_expr_t r = p->n_arrays ? p->arrays_head : EXPR_NULL; r != EXPR_NULL && !rc;) {
        const ArraySpec *a = dvs_pool_ptr(pool, r);
        if (awidth[a->array_id])
            rc = _fail(why, whylen, "BAD_VAR: array %u is declared twice", a->array_id);
        awidth[a->array_id] = a->elem_width;
        r = a->next;
    }

    /* All-different: variables of up to 32 bits. */
    for (dvs_expr_t r = p->allDiff_head; r != EXPR_NULL && !rc;) {
        const AllDiffSpec *ad = dvs_pool_ptr(pool, r);
        const uint32_t *ids = (const uint32_t *)(ad + 1);
        for (uint32_t i = 0; i < ad->n_vars && !rc; i++) {
            if (ids[i] & DVS_ARRAY_MEMBER_BIT) {
                uint32_t a = ids[i] & ~DVS_ARRAY_MEMBER_BIT;
                if (a >= p->n_arrays)
                    rc = _fail(why, whylen, "BAD_VAR: all-different names array %u, with "
                               "%u arrays", a, p->n_arrays);
                else if (awidth[a] > 32)
                    rc = _fail(why, whylen, "UNSUPPORTED_WIDTH: all-different over array "
                               "%u of %u bits, over 32", a, awidth[a]);
            } else if (ids[i] >= n)
                rc = _fail(why, whylen, "BAD_VAR: all-different names variable %u, with "
                           "%u variables", ids[i], n);
            else if (width[ids[i]] > 32)
                rc = _fail(why, whylen, "UNSUPPORTED_WIDTH: all-different over variable "
                           "%u of %u bits, over 32", ids[i], width[ids[i]]);
        }
        r = ad->next;
    }

    /* Every variable a source group or a distribution names is declared. */
    for (dvs_expr_t r = p->sources_head; r != EXPR_NULL && !rc;) {
        const SourceSpec *ss = dvs_pool_ptr(pool, r);
        const uint32_t *ids = (const uint32_t *)(ss + 1);
        for (uint32_t i = 0; i < ss->n_vars && !rc; i++)
            if (ids[i] >= n)
                rc = _fail(why, whylen, "BAD_VAR: a source group names variable %u, with "
                           "%u variables", ids[i], n);
        r = ss->next;
    }
    for (dvs_expr_t r = p->dists_head; r != EXPR_NULL && !rc;) {
        const DistSpec *ds = dvs_pool_ptr(pool, r);
        if (ds->var_id >= n)
            rc = _fail(why, whylen, "BAD_VAR: a distribution names variable %u, with %u "
                       "variables", ds->var_id, n);
        r = ds->next;
    }
    free(width);
    free(awidth);
    if (rc) return rc;

    /* The expressions, as SV elaboration (and the check of the names an
     * added constraint uses) refuses them: an undeclared variable is
     * BAD_VAR; a width over DVS_MAX_WIDTH, a reversed slice or nesting
     * deeper than DVS_SV_MAX_DEPTH is UNSUPPORTED_WIDTH. The image check
     * passed, so every node a root reaches is in range and lies below its
     * parent: mark the reachable ones from the top down, then take their
     * depths from the bottom up. */
    const uint8_t *base = (const uint8_t *)pool;
    const uint64_t end = sizeof(dvs_pool_t) + (uint64_t)pool->used;
    const uint32_t n_slots = (uint32_t)((end - sizeof(dvs_pool_t) + DVS_IMAGE_ALIGN - 1)
                                        / DVS_IMAGE_ALIGN);
    uint32_t *depth = calloc(n_slots ? n_slots : 1, sizeof *depth);
    if (!depth) return _fail(why, whylen, "out of memory");
#define SLOT(r)  (((r) - (uint32_t)sizeof(dvs_pool_t)) / DVS_IMAGE_ALIGN)
    for (dvs_expr_t r = p->constraints_head; r != EXPR_NULL;) {
        const ConstraintSpec *cs = dvs_pool_ptr(pool, r);
        depth[SLOT(cs->root)] = 1;
        r = cs->next;
    }
    for (dvs_expr_t r = p->softs_head; r != EXPR_NULL;) {
        const SoftSpec *ss = dvs_pool_ptr(pool, r);
        depth[SLOT(ss->root)] = 1;
        r = ss->next;
    }
    for (dvs_expr_t f = p->n_foreachs ? p->foreachs_head : EXPR_NULL; f != EXPR_NULL;) {
        const ForeachSpec *fe = dvs_pool_ptr(pool, f);
        for (dvs_expr_t r = fe->n_constraints ? fe->constraints_head : EXPR_NULL;
             r != EXPR_NULL;) {
            const ConstraintSpec *cs = dvs_pool_ptr(pool, r);
            depth[SLOT(cs->root)] = 1;
            r = cs->next;
        }
        for (dvs_expr_t r = fe->n_softs ? fe->softs_head : EXPR_NULL; r != EXPR_NULL;) {
            const SoftSpec *ss = dvs_pool_ptr(pool, r);
            depth[SLOT(ss->root)] = 1;
            r = ss->next;
        }
        for (dvs_expr_t r = fe->n_dists ? fe->dists_head : EXPR_NULL; r != EXPR_NULL;) {
            const DistSpec *ds = dvs_pool_ptr(pool, r);
            depth[SLOT(ds->index)] = 1;
            r = ds->next;
        }
        f = fe->next;
    }
    image_node_t nd;
    for (uint32_t s = n_slots; s-- > 0;) {
        if (!depth[s]) continue;
        uint32_t r = (uint32_t)sizeof(dvs_pool_t) + s * DVS_IMAGE_ALIGN;
        dvs_image_node(base + r, end - r, &nd);
        for (uint64_t i = 0; i < nd.n_kids + nd.n_more; i++)
            depth[SLOT(i < nd.n_kids ? nd.kids[i] : nd.more[i - nd.n_kids])] = 1;
    }
    for (uint32_t s = 0; s < n_slots && !rc; s++) {
        if (!depth[s]) continue;
        uint32_t r = (uint32_t)sizeof(dvs_pool_t) + s * DVS_IMAGE_ALIGN;
        const void *node = base + r;
        dvs_image_node(node, end - r, &nd);
        uint32_t d = 0;
        for (uint64_t i = 0; i < nd.n_kids + nd.n_more; i++) {
            uint32_t k = depth[SLOT(i < nd.n_kids ? nd.kids[i] : nd.more[i - nd.n_kids])];
            if (k > d) d = k;
        }
        depth[s] = d + 1;
        if (depth[s] > DVS_SV_MAX_DEPTH)
            rc = _fail(why, whylen, "UNSUPPORTED_WIDTH: expressions nest deeper than %u",
                       DVS_SV_MAX_DEPTH);
        else if (*(const uint32_t *)node == EXPR_VAR
                 && ((const ExprVar *)node)->var_id >= n)
            rc = _fail(why, whylen, "BAD_VAR: an expression names variable %u, with %u "
                       "variables", ((const ExprVar *)node)->var_id, n);
        else if (*(const uint32_t *)node == EXPR_ARRAY_SELECT) {
            const ExprArraySelect *a = node;
            if ((uint64_t)a->base_var_id + a->n_elems > n)
                rc = _fail(why, whylen, "BAD_VAR: an array select names variables %u..%u, "
                           "with %u variables", a->base_var_id,
                           a->base_var_id + a->n_elems - 1, n);
        } else
            rc = _node_widths(node, why, whylen, r);
    }
#undef SLOT
    free(depth);
    return rc;
}
