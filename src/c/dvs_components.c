#include <stdlib.h>
#include <string.h>
#include "dvs_components.h"

int dvs_uf_init(dvs_uf_t *uf, uint32_t n) {
    uf->n = n;
    uf->parent = (uint32_t *)malloc((n ? n : 1) * sizeof(uint32_t));
    if (!uf->parent) return 0;
    for (uint32_t i = 0; i < n; i++) uf->parent[i] = i;
    return 1;
}

void dvs_uf_free(dvs_uf_t *uf) {
    free(uf->parent);
    uf->parent = NULL;
    uf->n = 0;
}

uint32_t dvs_uf_find(dvs_uf_t *uf, uint32_t x) {
    uint32_t r = x;
    while (uf->parent[r] != r) r = uf->parent[r];
    while (uf->parent[x] != r) { uint32_t nx = uf->parent[x]; uf->parent[x] = r; x = nx; }
    return r;
}

void dvs_uf_union(dvs_uf_t *uf, uint32_t a, uint32_t b) {
    a = dvs_uf_find(uf, a);
    b = dvs_uf_find(uf, b);
    if (a == b) return;
    /* Keep the lower id as root so numbering is deterministic. */
    if (a < b) uf->parent[b] = a; else uf->parent[a] = b;
}

int dvs_components_from_uf(dvs_components_t *out, dvs_uf_t *uf,
                           const uint8_t *used, uint32_t n_vars) {
    memset(out, 0, sizeof(*out));
    out->var_comp = (uint32_t *)malloc((n_vars ? n_vars : 1) * sizeof(uint32_t));
    uint32_t *id_of = (uint32_t *)malloc((uf->n ? uf->n : 1) * sizeof(uint32_t));
    uint32_t *size = (uint32_t *)calloc(n_vars ? n_vars : 1, sizeof(uint32_t));
    if (!out->var_comp || !id_of || !size) {
        free(out->var_comp); free(id_of); free(size);
        out->var_comp = NULL;
        return 0;
    }
    for (uint32_t i = 0; i < uf->n; i++) id_of[i] = UINT32_MAX;
    out->n_vars = n_vars;
    for (uint32_t v = 0; v < n_vars; v++) {
        if (!used[v]) { out->var_comp[v] = UINT32_MAX; continue; }
        uint32_t r = dvs_uf_find(uf, v);
        if (id_of[r] == UINT32_MAX) id_of[r] = out->n_comp++;
        uint32_t c = id_of[r];
        out->var_comp[v] = c;
        if (++size[c] > out->largest) out->largest = size[c];
    }
    free(id_of);
    free(size);
    return 1;
}

void dvs_components_free(dvs_components_t *c) {
    free(c->var_comp);
    memset(c, 0, sizeof(*c));
}
