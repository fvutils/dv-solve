#include <stdlib.h>
#include <string.h>
#include "smt2/smt2_parser.h"

/* ------------------------------------------------------------------ */
/* Arena allocator                                                     */
/* ------------------------------------------------------------------ */

#define ARENA_DEFAULT_BLOCK  (4096u)
#define ARENA_BLOCK_DATA(b)  ((uint8_t *)((b) + 1))

void sexpr_arena_init(SexprArena *arena, size_t block_size) {
    arena->head       = NULL;
    arena->current    = NULL;
    arena->block_size = block_size > 0 ? block_size : ARENA_DEFAULT_BLOCK;
}

static SexprArenaBlock *_arena_new_block(size_t capacity) {
    SexprArenaBlock *b = (SexprArenaBlock *)malloc(sizeof(SexprArenaBlock) + capacity);
    if (!b) return NULL;
    b->next     = NULL;
    b->capacity = capacity;
    b->used     = 0;
    return b;
}

void *sexpr_arena_alloc(SexprArena *arena, size_t bytes, size_t align) {
    if (align < 1) align = 1;

    /* Try current block first */
    if (arena->current) {
        size_t off  = arena->current->used;
        size_t pad  = (align - (off % align)) % align;
        size_t need = pad + bytes;
        if (off + need <= arena->current->capacity) {
            void *ptr = ARENA_BLOCK_DATA(arena->current) + off + pad;
            arena->current->used = off + need;
            return ptr;
        }
    }

    /* Need a new block */
    size_t cap = arena->block_size;
    if (bytes + align > cap) cap = bytes + align;
    SexprArenaBlock *nb = _arena_new_block(cap);
    if (!nb) return NULL;

    if (arena->current)
        arena->current->next = nb;
    else
        arena->head = nb;
    arena->current = nb;

    size_t pad  = (align - (0 % align)) % align; /* always 0 for fresh block */
    void *ptr   = ARENA_BLOCK_DATA(nb) + pad;
    nb->used    = pad + bytes;
    return ptr;
}

void sexpr_arena_destroy(SexprArena *arena) {
    SexprArenaBlock *b = arena->head;
    while (b) {
        SexprArenaBlock *n = b->next;
        free(b);
        b = n;
    }
    arena->head    = NULL;
    arena->current = NULL;
}

void sexpr_arena_reset(SexprArena *arena) {
    SexprArenaBlock *b = arena->head;
    while (b) {
        b->used = 0;
        b = b->next;
    }
    arena->current = arena->head;
}

/* ------------------------------------------------------------------ */
/* Parser internals                                                    */
/* ------------------------------------------------------------------ */

/** Temporary growable list for collecting child nodes. */
typedef struct {
    Sexpr **items;
    uint32_t count;
    uint32_t capacity;
} SexprList;

static void _list_init(SexprList *l) {
    l->items    = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static int _list_push(SexprList *l, Sexpr *s) {
    if (l->count == l->capacity) {
        uint32_t newcap = l->capacity ? l->capacity * 2 : 8;
        Sexpr **tmp = (Sexpr **)realloc(l->items, newcap * sizeof(Sexpr *));
        if (!tmp) return -1;
        l->items    = tmp;
        l->capacity = newcap;
    }
    l->items[l->count++] = s;
    return 0;
}

static void _list_free(SexprList *l) {
    free(l->items);
    l->items    = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static Sexpr *_alloc_sexpr(SexprArena *arena) {
    return (Sexpr *)sexpr_arena_alloc(arena, sizeof(Sexpr), _Alignof(Sexpr));
}

/** Parse one atom or list from the lexer (recursive). */
static Sexpr *_parse_one(Smt2Lexer *lex, SexprArena *arena) {
    Smt2Token tok = smt2_lexer_next(lex);

    switch (tok.kind) {
    case TOK_EOF:
    case TOK_ERROR:
        return NULL;

    case TOK_RPAREN:
        /* Unmatched ')' -- caller should not see this. */
        return NULL;

    case TOK_SYMBOL: {
        Sexpr *s = _alloc_sexpr(arena);
        if (!s) return NULL;
        s->kind    = SEXPR_SYMBOL;
        s->sym.str = tok.start;
        s->sym.len = tok.length;
        return s;
    }
    case TOK_NUMERAL: {
        Sexpr *s = _alloc_sexpr(arena);
        if (!s) return NULL;
        s->kind   = SEXPR_NUMERAL;
        s->numval = tok.numval;
        return s;
    }
    case TOK_BITVEC_LIT: {
        Sexpr *s = _alloc_sexpr(arena);
        if (!s) return NULL;
        s->kind     = SEXPR_BITVEC;
        s->bv.value = tok.numval;
        s->bv.width = tok.bv_width;
        return s;
    }
    case TOK_KEYWORD: {
        Sexpr *s = _alloc_sexpr(arena);
        if (!s) return NULL;
        s->kind    = SEXPR_KEYWORD;
        s->sym.str = tok.start;
        s->sym.len = tok.length;
        return s;
    }
    case TOK_STRING: {
        Sexpr *s = _alloc_sexpr(arena);
        if (!s) return NULL;
        s->kind    = SEXPR_STRING;
        s->sym.str = tok.start;
        s->sym.len = tok.length;
        return s;
    }
    case TOK_LPAREN: {
        /* Parse list: children until ')' */
        SexprList children;
        _list_init(&children);

        for (;;) {
            Smt2Token peek = smt2_lexer_peek(lex);
            if (peek.kind == TOK_RPAREN) {
                smt2_lexer_next(lex); /* consume ')' */
                break;
            }
            if (peek.kind == TOK_EOF || peek.kind == TOK_ERROR) {
                _list_free(&children);
                return NULL;
            }
            Sexpr *child = _parse_one(lex, arena);
            if (!child) {
                _list_free(&children);
                return NULL;
            }
            if (_list_push(&children, child) < 0) {
                _list_free(&children);
                return NULL;
            }
        }

        Sexpr *s = _alloc_sexpr(arena);
        if (!s) { _list_free(&children); return NULL; }
        s->kind       = SEXPR_LIST;
        s->list.count = children.count;

        /* Copy the pointer array into the arena */
        if (children.count > 0) {
            size_t sz = children.count * sizeof(Sexpr *);
            Sexpr **arr = (Sexpr **)sexpr_arena_alloc(arena, sz,
                                                       _Alignof(Sexpr *));
            if (!arr) { _list_free(&children); return NULL; }
            memcpy(arr, children.items, sz);
            s->list.items = arr;
        } else {
            s->list.items = NULL;
        }
        _list_free(&children);
        return s;
    }
    }
    return NULL; /* unreachable */
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

Sexpr *sexpr_parse(Smt2Lexer *lex, SexprArena *arena) {
    /* Skip to the next meaningful token */
    Smt2Token peek = smt2_lexer_peek(lex);
    if (peek.kind == TOK_EOF) return NULL;
    return _parse_one(lex, arena);
}

int sexpr_is_symbol(const Sexpr *s, const char *str) {
    if (!s || s->kind != SEXPR_SYMBOL) return 0;
    size_t slen = strlen(str);
    if (s->sym.len != (uint32_t)slen) return 0;
    return memcmp(s->sym.str, str, slen) == 0;
}

int sexpr_is_command(const Sexpr *s, const char *cmd) {
    if (!s || s->kind != SEXPR_LIST || s->list.count < 1) return 0;
    return sexpr_is_symbol(s->list.items[0], cmd);
}

int sexpr_is_keyword(const Sexpr *s, const char *str) {
    if (!s || s->kind != SEXPR_KEYWORD) return 0;
    size_t slen = strlen(str);
    if (s->sym.len != (uint32_t)slen) return 0;
    return memcmp(s->sym.str, str, slen) == 0;
}
