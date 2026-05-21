#ifndef SMT2_LEXER_H
#define SMT2_LEXER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Token types                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    TOK_LPAREN,      /* (                                     */
    TOK_RPAREN,      /* )                                     */
    TOK_SYMBOL,      /* bvadd, declare-const, x, _, ...       */
    TOK_NUMERAL,     /* 42, 0                                 */
    TOK_BITVEC_LIT,  /* #b0101, #xFF                          */
    TOK_STRING,      /* "..."                                 */
    TOK_KEYWORD,     /* :produce-models, :seed                */
    TOK_EOF,
    TOK_ERROR,
} Smt2TokenKind;

/* ------------------------------------------------------------------ */
/* Token value                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    Smt2TokenKind kind;
    const char   *start;    /* pointer into input buffer            */
    uint32_t      length;   /* token length in bytes                */
    uint64_t      numval;   /* parsed numeric value (NUMERAL/BITVEC_LIT) */
    uint32_t      bv_width; /* bit width (BITVEC_LIT only)         */
} Smt2Token;

/* ------------------------------------------------------------------ */
/* Lexer state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *buf;      /* input buffer (not owned)              */
    size_t      buf_len;  /* total length of input                 */
    size_t      pos;      /* current read position                 */
    uint32_t    line;     /* 1-based line number                   */
    uint32_t    col;      /* 1-based column number                 */
} Smt2Lexer;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * Initialise a lexer over a contiguous buffer.
 *
 * The buffer is NOT copied -- the caller must keep it alive for the
 * lifetime of the lexer and any tokens it produces (token.start points
 * into this buffer).
 */
void smt2_lexer_init(Smt2Lexer *lex, const char *buf, size_t len);

/**
 * Read the next token.
 *
 * Returns a token by value.  After EOF the lexer keeps returning
 * TOK_EOF.  On a lexical error it returns TOK_ERROR with start/length
 * pointing at the offending byte(s).
 */
Smt2Token smt2_lexer_next(Smt2Lexer *lex);

/**
 * Peek at the next token without consuming it.
 *
 * The internal position is saved and restored.
 */
Smt2Token smt2_lexer_peek(Smt2Lexer *lex);

/**
 * Return a human-readable name for a token kind (for diagnostics).
 */
const char *smt2_token_kind_name(Smt2TokenKind kind);

/**
 * Compare a token's text to a NUL-terminated string.
 * Returns non-zero if they match.
 */
int smt2_token_eq(const Smt2Token *tok, const char *str);

#ifdef __cplusplus
}
#endif

#endif /* SMT2_LEXER_H */
