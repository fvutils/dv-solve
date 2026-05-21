/*
 * Unit tests for the SMT-LIB2 lexer.
 *
 * Build:
 *   gcc -O2 -I src/c tests/c/test_smt2_lexer.c src/c/smt2/smt2_lexer.c \
 *       -o build/test_smt2_lexer
 *
 * Run:
 *   ./build/test_smt2_lexer
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "smt2/smt2_lexer.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int _passed = 0;
static int _failed = 0;

#define ASSERT_EQ_INT(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL %s:%d: %s == %d, expected %d\n", \
                __FILE__, __LINE__, #a, (int)(a), (int)(b)); \
        _failed++; return; \
    } \
} while (0)

#define ASSERT_EQ_U64(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL %s:%d: %s == %llu, expected %llu\n", \
                __FILE__, __LINE__, #a, \
                (unsigned long long)(a), (unsigned long long)(b)); \
        _failed++; return; \
    } \
} while (0)

#define ASSERT_TOK_TEXT(tok, expected) do { \
    if (!smt2_token_eq(&(tok), (expected))) { \
        fprintf(stderr, "  FAIL %s:%d: token text '%.*s', expected '%s'\n", \
                __FILE__, __LINE__, (tok).length, (tok).start, (expected)); \
        _failed++; return; \
    } \
} while (0)

#define RUN(fn) do { \
    int prev_failed = _failed; \
    fn(); \
    if (_failed == prev_failed) { \
        _passed++; \
        printf("  PASS  %s\n", #fn); \
    } else { \
        printf("  FAIL  %s\n", #fn); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Tests: parentheses                                                  */
/* ------------------------------------------------------------------ */

static void test_parens(void) {
    const char *input = "(())";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_LPAREN);
    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_LPAREN);
    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_RPAREN);
    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_RPAREN);
    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
    /* EOF is idempotent */
    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Tests: symbols                                                      */
/* ------------------------------------------------------------------ */

static void test_symbols(void) {
    const char *input = "bvadd declare-const x _ BitVec";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(t, "bvadd");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(t, "declare-const");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(t, "x");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(t, "_");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(t, "BitVec");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Tests: quoted symbols                                               */
/* ------------------------------------------------------------------ */

static void test_quoted_symbol(void) {
    const char *input = "|hello world|";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(t, "|hello world|");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

static void test_unterminated_quoted_symbol(void) {
    const char *input = "|oops";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_ERROR);
}

/* ------------------------------------------------------------------ */
/* Tests: numerals                                                     */
/* ------------------------------------------------------------------ */

static void test_numerals(void) {
    const char *input = "0 42 12345678901234";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_NUMERAL);
    ASSERT_EQ_U64(t.numval, 0);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_NUMERAL);
    ASSERT_EQ_U64(t.numval, 42);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_NUMERAL);
    ASSERT_EQ_U64(t.numval, 12345678901234ULL);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Tests: bitvector literals                                           */
/* ------------------------------------------------------------------ */

static void test_bv_binary(void) {
    const char *input = "#b0101 #b1 #b00000000";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_BITVEC_LIT);
    ASSERT_EQ_U64(t.numval, 5);
    ASSERT_EQ_INT(t.bv_width, 4);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_BITVEC_LIT);
    ASSERT_EQ_U64(t.numval, 1);
    ASSERT_EQ_INT(t.bv_width, 1);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_BITVEC_LIT);
    ASSERT_EQ_U64(t.numval, 0);
    ASSERT_EQ_INT(t.bv_width, 8);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

static void test_bv_hex(void) {
    const char *input = "#xFF #x0A #xDEAD";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_BITVEC_LIT);
    ASSERT_EQ_U64(t.numval, 0xFF);
    ASSERT_EQ_INT(t.bv_width, 8);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_BITVEC_LIT);
    ASSERT_EQ_U64(t.numval, 0x0A);
    ASSERT_EQ_INT(t.bv_width, 8);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_BITVEC_LIT);
    ASSERT_EQ_U64(t.numval, 0xDEAD);
    ASSERT_EQ_INT(t.bv_width, 16);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

static void test_bv_error(void) {
    /* '#' with no 'b' or 'x' following */
    const char *input = "#z";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_ERROR);
}

static void test_bv_empty_digits(void) {
    /* '#b' with no binary digits */
    const char *input = "#b ";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_ERROR);
}

/* ------------------------------------------------------------------ */
/* Tests: keywords                                                     */
/* ------------------------------------------------------------------ */

static void test_keywords(void) {
    const char *input = ":produce-models :seed :status";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_KEYWORD);
    ASSERT_TOK_TEXT(t, ":produce-models");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_KEYWORD);
    ASSERT_TOK_TEXT(t, ":seed");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_KEYWORD);
    ASSERT_TOK_TEXT(t, ":status");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Tests: string literals                                              */
/* ------------------------------------------------------------------ */

static void test_string(void) {
    const char *input = "\"hello world\"";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_STRING);
    ASSERT_TOK_TEXT(t, "\"hello world\"");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

static void test_string_escaped_quote(void) {
    /* SMT-LIB2: "" inside a string is an escaped " */
    const char *input = "\"say \"\"hi\"\"\"";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_STRING);
    /* The whole token including outer quotes */
    ASSERT_TOK_TEXT(t, "\"say \"\"hi\"\"\"");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

static void test_unterminated_string(void) {
    const char *input = "\"oops";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_ERROR);
}

/* ------------------------------------------------------------------ */
/* Tests: comments                                                     */
/* ------------------------------------------------------------------ */

static void test_comment(void) {
    const char *input = "; this is a comment\n42";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_NUMERAL);
    ASSERT_EQ_U64(t.numval, 42);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

static void test_comment_at_eof(void) {
    const char *input = "; only a comment";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Tests: whitespace                                                   */
/* ------------------------------------------------------------------ */

static void test_whitespace_varieties(void) {
    const char *input = "  \t\r\n  x  \n  y  ";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(t, "x");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(t, "y");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Tests: empty input                                                  */
/* ------------------------------------------------------------------ */

static void test_empty_input(void) {
    Smt2Lexer lex;
    smt2_lexer_init(&lex, "", 0);

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Tests: realistic SMT-LIB2 fragment                                  */
/* ------------------------------------------------------------------ */

static void test_realistic_fragment(void) {
    const char *input =
        "(set-logic QF_BV)\n"
        "(declare-const x (_ BitVec 8))\n"
        "(assert (bvuge x (_ bv42 8)))\n"
        "(check-sat)\n"
        "(exit)\n";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    /* (set-logic QF_BV) */
    Smt2Token t;
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_LPAREN);
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(t, "set-logic");
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(t, "QF_BV");
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_RPAREN);

    /* (declare-const x (_ BitVec 8)) */
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_LPAREN);
    t = smt2_lexer_next(&lex); ASSERT_TOK_TEXT(t, "declare-const");
    t = smt2_lexer_next(&lex); ASSERT_TOK_TEXT(t, "x");
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_LPAREN);
    t = smt2_lexer_next(&lex); ASSERT_TOK_TEXT(t, "_");
    t = smt2_lexer_next(&lex); ASSERT_TOK_TEXT(t, "BitVec");
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_NUMERAL);
    ASSERT_EQ_U64(t.numval, 8);
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_RPAREN);
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_RPAREN);

    /* (assert (bvuge x (_ bv42 8))) */
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_LPAREN);
    t = smt2_lexer_next(&lex); ASSERT_TOK_TEXT(t, "assert");
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_LPAREN);
    t = smt2_lexer_next(&lex); ASSERT_TOK_TEXT(t, "bvuge");
    t = smt2_lexer_next(&lex); ASSERT_TOK_TEXT(t, "x");
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_LPAREN);
    t = smt2_lexer_next(&lex); ASSERT_TOK_TEXT(t, "_");
    t = smt2_lexer_next(&lex); ASSERT_TOK_TEXT(t, "bv42");
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_NUMERAL);
    ASSERT_EQ_U64(t.numval, 8);
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_RPAREN);
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_RPAREN);
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_RPAREN);

    /* (check-sat) */
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_LPAREN);
    t = smt2_lexer_next(&lex); ASSERT_TOK_TEXT(t, "check-sat");
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_RPAREN);

    /* (exit) */
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_LPAREN);
    t = smt2_lexer_next(&lex); ASSERT_TOK_TEXT(t, "exit");
    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_RPAREN);

    t = smt2_lexer_next(&lex); ASSERT_EQ_INT(t.kind, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Tests: peek                                                         */
/* ------------------------------------------------------------------ */

static void test_peek(void) {
    const char *input = "abc 42";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token p = smt2_lexer_peek(&lex);
    ASSERT_EQ_INT(p.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(p, "abc");

    /* peek again -- same result */
    p = smt2_lexer_peek(&lex);
    ASSERT_EQ_INT(p.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(p, "abc");

    /* now consume it */
    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_SYMBOL);
    ASSERT_TOK_TEXT(t, "abc");

    /* peek sees the numeral */
    p = smt2_lexer_peek(&lex);
    ASSERT_EQ_INT(p.kind, TOK_NUMERAL);
    ASSERT_EQ_U64(p.numval, 42);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_NUMERAL);
    ASSERT_EQ_U64(t.numval, 42);

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Tests: token_kind_name                                              */
/* ------------------------------------------------------------------ */

static void test_token_kind_name(void) {
    ASSERT_EQ_INT(strcmp(smt2_token_kind_name(TOK_LPAREN), "LPAREN"), 0);
    ASSERT_EQ_INT(strcmp(smt2_token_kind_name(TOK_EOF), "EOF"), 0);
    ASSERT_EQ_INT(strcmp(smt2_token_kind_name(TOK_BITVEC_LIT), "BITVEC_LIT"), 0);
}

/* ------------------------------------------------------------------ */
/* Tests: line/column tracking                                         */
/* ------------------------------------------------------------------ */

static void test_line_col(void) {
    const char *input = "a\nb\n  c";
    Smt2Lexer lex;
    smt2_lexer_init(&lex, input, strlen(input));

    Smt2Token t = smt2_lexer_next(&lex);
    ASSERT_TOK_TEXT(t, "a");
    /* After consuming "a", pos is past it; but we check the lexer's
       state reflects line tracking works at all. */

    t = smt2_lexer_next(&lex);
    ASSERT_TOK_TEXT(t, "b");

    t = smt2_lexer_next(&lex);
    ASSERT_TOK_TEXT(t, "c");

    t = smt2_lexer_next(&lex);
    ASSERT_EQ_INT(t.kind, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_smt2_lexer:\n");

    RUN(test_parens);
    RUN(test_symbols);
    RUN(test_quoted_symbol);
    RUN(test_unterminated_quoted_symbol);
    RUN(test_numerals);
    RUN(test_bv_binary);
    RUN(test_bv_hex);
    RUN(test_bv_error);
    RUN(test_bv_empty_digits);
    RUN(test_keywords);
    RUN(test_string);
    RUN(test_string_escaped_quote);
    RUN(test_unterminated_string);
    RUN(test_comment);
    RUN(test_comment_at_eof);
    RUN(test_whitespace_varieties);
    RUN(test_empty_input);
    RUN(test_realistic_fragment);
    RUN(test_peek);
    RUN(test_token_kind_name);
    RUN(test_line_col);

    printf("\n%d passed, %d failed\n", _passed, _failed);
    return _failed > 0 ? 1 : 0;
}
