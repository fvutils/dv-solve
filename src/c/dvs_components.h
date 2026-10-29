/* Union-find over variable/constraint ids, used to split a problem into
 * independent components (L0 decomposition). */
#ifndef DVS_COMPONENTS_H
#define DVS_COMPONENTS_H
#include <stdint.h>

typedef struct {
    uint32_t *parent;
    uint32_t  n;
} dvs_uf_t;

/* Per-variable component ids for one encode; var_comp[v] < n_comp, or
 * UINT32_MAX for a variable no kept constraint reads. */
typedef struct {
    uint32_t *var_comp;
    uint32_t  n_vars;
    uint32_t  n_comp;
    uint32_t  largest;     /* variables in the largest component */
} dvs_components_t;

int      dvs_uf_init(dvs_uf_t *uf, uint32_t n);
void     dvs_uf_free(dvs_uf_t *uf);
uint32_t dvs_uf_find(dvs_uf_t *uf, uint32_t x);
void     dvs_uf_union(dvs_uf_t *uf, uint32_t a, uint32_t b);

/* Number vars [0, n_vars) by their root in uf; used[v]==0 marks a var no
 * constraint reads. Returns 0 on allocation failure. */
int      dvs_components_from_uf(dvs_components_t *out, dvs_uf_t *uf,
                                const uint8_t *used, uint32_t n_vars);
void     dvs_components_free(dvs_components_t *c);

#endif
