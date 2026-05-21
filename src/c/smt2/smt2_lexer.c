#include <string.h>
#include <ctype.h>
#include "smt2_lexer.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline int _at_end(const Smt2Lexer *lex) {
    return lex->pos >= lex->buf_len;
}

static inline char _peek_char(const Smt2Lexer *lex) {
    if (_at_end(lex)) return '\0';
    return lex->buf[lex->pos];
}

static inline char _advance(Smt2Lexer *lex) {
    char c = lex->buf[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    return c;
}

/** Is c a character that terminates a symbol/numeral? */
static inline int _is_delimiter(char c) {
    return c == '\0' || c == '(' || c == ')' || c == ';'
        || c == '"'  || isspace((unsigned char)c);
}

static Smt2Token _make_token(Smt2TokenKind kind, const char *start,
                              uint32_t length) {
    Smt2Token t;
    t.kind     = kind;
    t.start    = start;
    t.length   = length;
    t.numval   = 0;
    t.bv_width = 0;
    return t;
}

/* ------------------------------------------------------------------ */
/* Skip whitespace and comments                                        */
/* ------------------------------------------------------------------ */

static void _skip_ws(Smt2Lexer *lex) {
    while (!_at_end(lex)) {
        char c = _peek_char(lex);
        if (c == ';') {
            /* comment: skip to end of line */
            while (!_at_end(lex) && _peek_char(lex) != '\n')
                _advance(lex);
            continue; /* will pick up the \n or EOF on the next iteration */
        }
        if (isspace((unsigned char)c)) {
            _advance(lex);
            continue;
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Numeral                                                             */
/* ------------------------------------------------------------------ */

static Smt2Token _lex_numeral(Smt2Lexer *lex) {
    const char *start = lex->buf + lex->pos;
    uint64_t val = 0;
    while (!_at_end(lex) && isdigit((unsigned char)_peek_char(lex))) {
        val = val * 10 + (uint64_t)(_peek_char(lex) - '0');
        _advance(lex);
    }
    uint32_t len = (uint32_t)(lex->buf + lex->pos - start);
    Smt2Token t  = _make_token(TOK_NUMERAL, start, len);
    t.numval     = val;
    return t;
}

/* ------------------------------------------------------------------ */
/* Bitvector literals: #b... and #x...                                 */
/* ------------------------------------------------------------------ */

static Smt2Token _lex_bitvec(Smt2Lexer *lex) {
    const char *start = lex->buf + lex->pos;
    _advance(lex); /* skip '#' */

    if (_at_end(lex))
        return _make_token(TOK_ERROR, start, 1);

    char fmt = _peek_char(lex);
    _advance(lex); /* skip 'b' or 'x' */

    uint64_t val   = 0;
    uint32_t width = 0;

    if (fmt == 'b' || fmt == 'B') {
        while (!_at_end(lex)) {
            char c = _peek_char(lex);
            if (c != '0' && c != '1') break;
            val = (val << 1) | (uint64_t)(c - '0');
            width++;
            _advance(lex);
        }
        if (width == 0)
            return _make_token(TOK_ERROR, start,
                               (uint32_t)(lex->buf + lex->pos - start));
    } else if (fmt == 'x' || fmt == 'X') {
        while (!_at_end(lex)) {
            char c = _peek_char(lex);
            uint64_t nibble;
            if (c >= '0' && c <= '9')      nibble = (uint64_t)(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = (uint64_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nibble = (uint64_t)(c - 'A' + 10);
            else break;
            val = (val << 4) | nibble;
            width += 4;
            _advance(lex);
        }
        if (width == 0)
            return _make_token(TOK_ERROR, start,
                               (uint32_t)(lex->buf + lex->pos - start));
    } else {
        return _make_token(TOK_ERROR, start,
                           (uint32_t)(lex->buf + lex->pos - start));
    }

    uint32_t len = (uint32_t)(lex->buf + lex->pos - start);
    Smt2Token t  = _make_token(TOK_BITVEC_LIT, start, len);
    t.numval     = val;
    t.bv_width   = width;
    return t;
}

/* ------------------------------------------------------------------ */
/* String literal: "..."                                               */
/* ------------------------------------------------------------------ */

static Smt2Token _lex_string(Smt2Lexer *lex) {
    const char *start = lex->buf + lex->pos;
    _advance(lex); /* skip opening '"' */

    while (!_at_end(lex)) {
        char c = _peek_char(lex);
        if (c == '"') {
            /* SMT-LIB2 escapes "" inside strings */
            _advance(lex);
            if (!_at_end(lex) && _peek_char(lex) == '"') {
                _advance(lex); /* consume escaped quote */
                continue;
            }
            /* end of string (closing quote already consumed) */
            uint32_t len = (uint32_t)(lex->buf + lex->pos - start);
            return _make_token(TOK_STRING, start, len);
        }
        _advance(lex);
    }
    /* unterminated string */
    uint32_t len = (uint32_t)(lex->buf + lex->pos - start);
    return _make_token(TOK_ERROR, start, len);
}

/* ------------------------------------------------------------------ */
/* Symbol / keyword                                                    */
/* ------------------------------------------------------------------ */

static Smt2Token _lex_symbol(Smt2Lexer *lex) {
    const char *start = lex->buf + lex->pos;
    while (!_at_end(lex) && !_is_delimiter(_peek_char(lex)))
        _advance(lex);
    uint32_t len = (uint32_t)(lex->buf + lex->pos - start);
    return _make_token(TOK_SYMBOL, start, len);
}

static Smt2Token _lex_keyword(Smt2Lexer *lex) {
    const char *start = lex->buf + lex->pos;
    _advance(lex); /* skip ':' */
    while (!_at_end(lex) && !_is_delimiter(_peek_char(lex)))
        _advance(lex);
    uint32_t len = (uint32_t)(lex->buf + lex->pos - start);
    return _make_token(TOK_KEYWORD, start, len);
}

/* Quoted symbol: |...| */
static Smt2Token _lex_quoted_symbol(Smt2Lexer *lex) {
    const char *start = lex->buf + lex->pos;
    _advance(lex); /* skip '|' */
    while (!_at_end(lex) && _peek_char(lex) != '|')
        _advance(lex);
    if (_at_end(lex))
        return _make_token(TOK_ERROR, start,
                           (uint32_t)(lex->buf + lex->pos - start));
    _advance(lex); /* skip closing '|' */
    uint32_t len = (uint32_t)(lex->buf + lex->pos - start);
    return _make_token(TOK_SYMBOL, start, len);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void smt2_lexer_init(Smt2Lexer *lex, const char *buf, size_t len) {
    lex->buf     = buf;
    lex->buf_len = len;
    lex->pos     = 0;
    lex->line    = 1;
    lex->col     = 1;
}

Smt2Token smt2_lexer_next(Smt2Lexer *lex) {
    _skip_ws(lex);

    if (_at_end(lex))
        return _make_token(TOK_EOF, lex->buf + lex->pos, 0);

    char c = _peek_char(lex);

    /* Single-character tokens */
    if (c == '(') {
        const char *s = lex->buf + lex->pos;
        _advance(lex);
        return _make_token(TOK_LPAREN, s, 1);
    }
    if (c == ')') {
        const char *s = lex->buf + lex->pos;
        _advance(lex);
        return _make_token(TOK_RPAREN, s, 1);
    }

    /* Bitvector literal */
    if (c == '#')
        return _lex_bitvec(lex);

    /* String literal */
    if (c == '"')
        return _lex_string(lex);

    /* Keyword */
    if (c == ':')
        return _lex_keyword(lex);

    /* Quoted symbol */
    if (c == '|')
        return _lex_quoted_symbol(lex);

    /* Numeral (starts with digit) */
    if (isdigit((unsigned char)c))
        return _lex_numeral(lex);

    /* Symbol (everything else) */
    return _lex_symbol(lex);
}

Smt2Token smt2_lexer_peek(Smt2Lexer *lex) {
    size_t   saved_pos  = lex->pos;
    uint32_t saved_line = lex->line;
    uint32_t saved_col  = lex->col;

    Smt2Token tok = smt2_lexer_next(lex);

    lex->pos  = saved_pos;
    lex->line = saved_line;
    lex->col  = saved_col;
    return tok;
}

const char *smt2_token_kind_name(Smt2TokenKind kind) {
    switch (kind) {
    case TOK_LPAREN:     return "LPAREN";
    case TOK_RPAREN:     return "RPAREN";
    case TOK_SYMBOL:     return "SYMBOL";
    case TOK_NUMERAL:    return "NUMERAL";
    case TOK_BITVEC_LIT: return "BITVEC_LIT";
    case TOK_STRING:     return "STRING";
    case TOK_KEYWORD:    return "KEYWORD";
    case TOK_EOF:        return "EOF";
    case TOK_ERROR:      return "ERROR";
    }
    return "UNKNOWN";
}

int smt2_token_eq(const Smt2Token *tok, const char *str) {
    size_t slen = strlen(str);
    if (tok->length != (uint32_t)slen) return 0;
    return memcmp(tok->start, str, slen) == 0;
}
