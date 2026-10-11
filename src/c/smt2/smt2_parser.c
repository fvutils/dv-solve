#include <ctype.h>
#include <stdio.h>
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

/** Temporary growable list for collecting child nodes. Most lists are short,
 *  so the first SEXPR_LIST_INLINE children live on the stack. */
#define SEXPR_LIST_INLINE 16
typedef struct {
    Sexpr **items;
    uint32_t count;
    uint32_t capacity;
    Sexpr   *inline_items[SEXPR_LIST_INLINE];
} SexprList;

static void _list_init(SexprList *l) {
    l->items    = l->inline_items;
    l->count    = 0;
    l->capacity = SEXPR_LIST_INLINE;
}

static int _list_push(SexprList *l, Sexpr *s) {
    if (l->count == l->capacity) {
        uint32_t newcap = l->capacity * 2;
        Sexpr **tmp;
        if (l->items == l->inline_items) {
            tmp = (Sexpr **)malloc(newcap * sizeof(Sexpr *));
            if (tmp) memcpy(tmp, l->items, l->count * sizeof(Sexpr *));
        } else {
            tmp = (Sexpr **)realloc(l->items, newcap * sizeof(Sexpr *));
        }
        if (!tmp) return -1;
        l->items    = tmp;
        l->capacity = newcap;
    }
    l->items[l->count++] = s;
    return 0;
}

static void _list_free(SexprList *l) {
    if (l->items != l->inline_items) free(l->items);
    l->items    = l->inline_items;
    l->count    = 0;
    l->capacity = SEXPR_LIST_INLINE;
}

/* Is `t` (length n) a quoted symbol |x| whose body x is a simple symbol, and
 * so the same symbol as x? Reserved words are not simple symbols. */
static uint8_t _unquotable(const char *t, uint32_t n) {
    static const char *const reserved[] = {
        "_", "!", "as", "let", "exists", "forall", "match", "par",
        "NUMERAL", "DECIMAL", "STRING", "BINARY", "HEXADECIMAL" };
    if (n < 3 || t[0] != '|' || t[n - 1] != '|') return 0;
    const char *b = t + 1;
    uint32_t m = n - 2;
    if (isdigit((unsigned char)b[0])) return 0;
    for (uint32_t i = 0; i < m; i++)
        if (!b[i] || (!isalnum((unsigned char)b[i]) && !strchr("~!@$%^&*_-+=<>.?/", b[i])))
            return 0;
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++)
        if (strlen(reserved[i]) == m && !memcmp(reserved[i], b, m)) return 0;
    return 1;
}

static Sexpr *_alloc_sexpr(SexprArena *arena) {
    return (Sexpr *)sexpr_arena_alloc(arena, sizeof(Sexpr), _Alignof(Sexpr));
}

/* Record the first parse error of this s-expression (see Smt2Lexer.err_msg). */
static void _perr(Smt2Lexer *lex, uint32_t line, uint32_t col, const char *msg,
                  const Smt2Token *tok) {
    if (lex->err_msg) return;
    if (tok && tok->length)
        snprintf(lex->err_buf, sizeof lex->err_buf, "%s '%.*s'", msg,
                 (int)(tok->length > 40 ? 40 : tok->length), tok->start);
    else
        snprintf(lex->err_buf, sizeof lex->err_buf, "%s", msg);
    lex->err_msg  = lex->err_buf;
    lex->err_line = line;
    lex->err_col  = col;
}

static void _perr_oom(Smt2Lexer *lex) {
    _perr(lex, lex->line, lex->col, "out of memory while parsing", NULL);
}

/** Build the atom for the already-lexed `tok` (any token but '('). */
static Sexpr *_parse_atom(Smt2Lexer *lex, SexprArena *arena, Smt2Token tok) {

    switch (tok.kind) {
    case TOK_EOF:
        return NULL;
    case TOK_ERROR:
        _perr(lex, tok.line, tok.col, smt2_token_error(&tok), &tok);
        return NULL;

    case TOK_RPAREN:
        /* Only reached for a ')' with no '(' to close (a list consumes its own). */
        _perr(lex, tok.line, tok.col, "unexpected ')'", NULL);
        return NULL;

    case TOK_SYMBOL: {
        Sexpr *s = _alloc_sexpr(arena);
        if (!s) { _perr_oom(lex); return NULL; }
        s->kind    = SEXPR_SYMBOL;
        s->quoted  = _unquotable(tok.start, tok.length);
        s->sym.str = tok.start + s->quoted;
        s->sym.len = tok.length - 2u * s->quoted;
        return s;
    }
    case TOK_NUMERAL: {
        Sexpr *s = _alloc_sexpr(arena);
        if (!s) { _perr_oom(lex); return NULL; }
        s->kind   = SEXPR_NUMERAL;
        s->numval = tok.numval;
        return s;
    }
    case TOK_BITVEC_LIT: {
        Sexpr *s = _alloc_sexpr(arena);
        if (!s) { _perr_oom(lex); return NULL; }
        s->kind     = SEXPR_BITVEC;
        s->bv.value = tok.numval;
        s->bv.width = tok.bv_width;
        s->bv.limbs = NULL;
        if (tok.bv_width > 64) {
            /* The token's numval is only the low 64 bits; keep the full value
             * so the frontend never sees a silently truncated wide literal. */
            uint32_t nl = (tok.bv_width + 63u) / 64u;
            uint64_t *limbs = (uint64_t *)sexpr_arena_alloc(
                arena, nl * sizeof(uint64_t), _Alignof(uint64_t));
            if (!limbs) { _perr_oom(lex); return NULL; }
            if (smt2_bvlit_limbs(tok.start, tok.length, limbs, nl) != 0) {
                _perr(lex, tok.line, tok.col, "invalid bit-vector literal", &tok);
                return NULL;
            }
            s->bv.limbs = limbs;
        }
        return s;
    }
    case TOK_KEYWORD:
    case TOK_DECIMAL: {
        Sexpr *s = _alloc_sexpr(arena);
        if (!s) { _perr_oom(lex); return NULL; }
        s->kind    = tok.kind == TOK_KEYWORD ? SEXPR_KEYWORD : SEXPR_DECIMAL;
        s->sym.str = tok.start;
        s->sym.len = tok.length;
        return s;
    }
    case TOK_STRING: {
        Sexpr *s = _alloc_sexpr(arena);
        if (!s) { _perr_oom(lex); return NULL; }
        s->kind    = SEXPR_STRING;
        s->sym.str = tok.start;
        s->sym.len = tok.length;
        return s;
    }
    case TOK_LPAREN:
        break;   /* lists are _parse_tok's */
    }
    return NULL;
}

/** Move a finished list's children into an arena list node. */
static Sexpr *_finish_list(Smt2Lexer *lex, SexprArena *arena, SexprList *children) {
    Sexpr *s = _alloc_sexpr(arena);
    if (!s) { _perr_oom(lex); return NULL; }
    s->kind       = SEXPR_LIST;
    s->list.count = children->count;
    s->list.items = NULL;
    if (children->count > 0) {
        size_t sz = children->count * sizeof(Sexpr *);
        Sexpr **arr = (Sexpr **)sexpr_arena_alloc(arena, sz, _Alignof(Sexpr *));
        if (!arr) { _perr_oom(lex); return NULL; }
        memcpy(arr, children->items, sz);
        s->list.items = arr;
    }
    return s;
}

/** Parse one atom or list starting at the already-lexed `tok`.
 *
 *  Iterative: each open list is a frame on a heap stack, so nesting depth is
 *  bounded by memory, not by the calling thread's stack (recursion here took
 *  336 bytes a level and overflowed a 1 MB thread at about 3000 levels). */
static Sexpr *_parse_tok(Smt2Lexer *lex, SexprArena *arena, Smt2Token tok) {
    if (tok.kind != TOK_LPAREN) return _parse_atom(lex, arena, tok);

    SexprList *st = NULL;
    uint32_t n = 0, cap = 0;
    for (;;) {
        if (tok.kind == TOK_LPAREN) {
            if (n == cap) {
                uint32_t ncap = cap ? cap * 2 : 16;
                SexprList *t = (SexprList *)realloc(st, ncap * sizeof *st);
                if (!t) { _perr_oom(lex); goto fail; }
                /* A frame still on its inline buffer pointed into the old
                 * array. Only an inline frame has capacity SEXPR_LIST_INLINE
                 * (the first growth doubles it onto the heap). */
                for (uint32_t i = 0; i < n; i++)
                    if (t[i].capacity == SEXPR_LIST_INLINE) t[i].items = t[i].inline_items;
                st = t;
                cap = ncap;
            }
            _list_init(&st[n++]);
            tok = smt2_lexer_next(lex);
            continue;
        }
        Sexpr *done;
        if (tok.kind == TOK_RPAREN) {
            done = _finish_list(lex, arena, &st[n - 1]);
            _list_free(&st[--n]);
            if (!done) goto fail;
            if (n == 0) { free(st); return done; }
        } else if (tok.kind == TOK_EOF) {
            _perr(lex, lex->cmd_line, lex->cmd_col,
                  "unexpected end of input: '(' is never closed", NULL);
            goto fail;
        } else {
            done = _parse_atom(lex, arena, tok);
            if (!done) goto fail;
        }
        if (_list_push(&st[n - 1], done) < 0) { _perr_oom(lex); goto fail; }
        tok = smt2_lexer_next(lex);
    }

fail:
    for (uint32_t i = 0; i < n; i++) _list_free(&st[i]);
    free(st);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

Sexpr *sexpr_parse(Smt2Lexer *lex, SexprArena *arena) {
    lex->err_msg = NULL;
    /* Skip to the next meaningful token */
    Smt2Token peek = smt2_lexer_peek(lex);
    if (peek.kind == TOK_EOF) return NULL;
    uint32_t depth0 = lex->depth;
    Sexpr *s = _parse_tok(lex, arena, smt2_lexer_next(lex));
    if (!s && lex->err_msg) {
        /* Resynchronise: skip the rest of the malformed s-expression, so the
         * next call starts at the following command rather than mid-list. */
        while (lex->depth > depth0) {
            Smt2Token t = smt2_lexer_next(lex);
            if (t.kind == TOK_EOF) break;
        }
    }
    return s;
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
