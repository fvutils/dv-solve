#ifndef SMT2_PARSER_H
#define SMT2_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include "smt2/smt2_lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* S-expression node types                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    SEXPR_SYMBOL,
    SEXPR_NUMERAL,
    SEXPR_BITVEC,
    SEXPR_KEYWORD,
    SEXPR_STRING,
    SEXPR_LIST,
} SexprKind;

typedef struct Sexpr {
    SexprKind kind;
    union {
        struct { const char *str; uint32_t len; }        sym;   /* SYMBOL/KEYWORD/STRING */
        uint64_t                                         numval;/* NUMERAL */
        struct { uint64_t value; uint32_t width; }       bv;    /* BITVEC */
        struct { struct Sexpr **items; uint32_t count; }  list;  /* LIST */
    };
} Sexpr;

/* ------------------------------------------------------------------ */
/* Arena allocator (bump-pointer)                                      */
/* ------------------------------------------------------------------ */

typedef struct SexprArenaBlock {
    struct SexprArenaBlock *next;
    size_t                  capacity;
    size_t                  used;
    /* data follows */
} SexprArenaBlock;

typedef struct {
    SexprArenaBlock *head;
    SexprArenaBlock *current;
    size_t           block_size;
} SexprArena;

void  sexpr_arena_init(SexprArena *arena, size_t block_size);
void *sexpr_arena_alloc(SexprArena *arena, size_t bytes, size_t align);
void  sexpr_arena_destroy(SexprArena *arena);
void  sexpr_arena_reset(SexprArena *arena);

/* ------------------------------------------------------------------ */
/* Parser API                                                          */
/* ------------------------------------------------------------------ */

/**
 * Parse one top-level S-expression from the lexer.
 *
 * Returns NULL on EOF or parse error.  All Sexpr nodes and their
 * backing storage are allocated from the arena.
 */
Sexpr *sexpr_parse(Smt2Lexer *lex, SexprArena *arena);

/* ------------------------------------------------------------------ */
/* Convenience accessors                                               */
/* ------------------------------------------------------------------ */

/** True if s is a SYMBOL matching str. */
int sexpr_is_symbol(const Sexpr *s, const char *str);

/** True if s is a LIST with count >= 1 and items[0] is SYMBOL matching str. */
int sexpr_is_command(const Sexpr *s, const char *cmd);

/** True if s is a KEYWORD matching str (including the leading ':'). */
int sexpr_is_keyword(const Sexpr *s, const char *str);

#ifdef __cplusplus
}
#endif

#endif /* SMT2_PARSER_H */
