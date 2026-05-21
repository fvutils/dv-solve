/*
 * Unit tests for the SMT-LIB2 S-expression parser.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "smt2/smt2_lexer.h"
#include "smt2/smt2_parser.h"

static int _passed = 0;
static int _failed = 0;

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        _failed++; return; \
    } \
} while (0)

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

#define RUN(fn) do { \
    int prev = _failed; \
    fn(); \
    if (_failed == prev) { _passed++; printf("  PASS  %s\n", #fn); } \
    else printf("  FAIL  %s\n", #fn); \
} while (0)

/* ------------------------------------------------------------------ */

static void test_parse_atom_symbol(void) {
    const char *input = "hello";
    Smt2Lexer lex; smt2_lexer_init(&lex, input, strlen(input));
    SexprArena arena; sexpr_arena_init(&arena, 0);

    Sexpr *s = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->kind, SEXPR_SYMBOL);
    ASSERT_TRUE(sexpr_is_symbol(s, "hello"));

    sexpr_arena_destroy(&arena);
}

static void test_parse_atom_numeral(void) {
    const char *input = "42";
    Smt2Lexer lex; smt2_lexer_init(&lex, input, strlen(input));
    SexprArena arena; sexpr_arena_init(&arena, 0);

    Sexpr *s = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->kind, SEXPR_NUMERAL);
    ASSERT_EQ_U64(s->numval, 42);

    sexpr_arena_destroy(&arena);
}

static void test_parse_simple_list(void) {
    const char *input = "(bvadd x y)";
    Smt2Lexer lex; smt2_lexer_init(&lex, input, strlen(input));
    SexprArena arena; sexpr_arena_init(&arena, 0);

    Sexpr *s = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->kind, SEXPR_LIST);
    ASSERT_EQ_INT(s->list.count, 3);
    ASSERT_TRUE(sexpr_is_symbol(s->list.items[0], "bvadd"));
    ASSERT_TRUE(sexpr_is_symbol(s->list.items[1], "x"));
    ASSERT_TRUE(sexpr_is_symbol(s->list.items[2], "y"));
    ASSERT_TRUE(sexpr_is_command(s, "bvadd"));

    sexpr_arena_destroy(&arena);
}

static void test_parse_nested(void) {
    const char *input = "(assert (bvule (bvadd x y) z))";
    Smt2Lexer lex; smt2_lexer_init(&lex, input, strlen(input));
    SexprArena arena; sexpr_arena_init(&arena, 0);

    Sexpr *s = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->kind, SEXPR_LIST);
    ASSERT_EQ_INT(s->list.count, 2);
    ASSERT_TRUE(sexpr_is_command(s, "assert"));

    /* (bvule (bvadd x y) z) */
    Sexpr *inner = s->list.items[1];
    ASSERT_EQ_INT(inner->kind, SEXPR_LIST);
    ASSERT_EQ_INT(inner->list.count, 3);
    ASSERT_TRUE(sexpr_is_command(inner, "bvule"));

    /* (bvadd x y) */
    Sexpr *add = inner->list.items[1];
    ASSERT_EQ_INT(add->kind, SEXPR_LIST);
    ASSERT_TRUE(sexpr_is_command(add, "bvadd"));

    sexpr_arena_destroy(&arena);
}

static void test_parse_indexed(void) {
    const char *input = "(_ BitVec 8)";
    Smt2Lexer lex; smt2_lexer_init(&lex, input, strlen(input));
    SexprArena arena; sexpr_arena_init(&arena, 0);

    Sexpr *s = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->kind, SEXPR_LIST);
    ASSERT_EQ_INT(s->list.count, 3);
    ASSERT_TRUE(sexpr_is_symbol(s->list.items[0], "_"));
    ASSERT_TRUE(sexpr_is_symbol(s->list.items[1], "BitVec"));
    ASSERT_EQ_INT(s->list.items[2]->kind, SEXPR_NUMERAL);
    ASSERT_EQ_U64(s->list.items[2]->numval, 8);

    sexpr_arena_destroy(&arena);
}

static void test_parse_bv_literal_indexed(void) {
    const char *input = "(_ bv42 8)";
    Smt2Lexer lex; smt2_lexer_init(&lex, input, strlen(input));
    SexprArena arena; sexpr_arena_init(&arena, 0);

    Sexpr *s = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->kind, SEXPR_LIST);
    ASSERT_EQ_INT(s->list.count, 3);
    ASSERT_TRUE(sexpr_is_symbol(s->list.items[0], "_"));
    ASSERT_TRUE(sexpr_is_symbol(s->list.items[1], "bv42"));
    ASSERT_EQ_U64(s->list.items[2]->numval, 8);

    sexpr_arena_destroy(&arena);
}

static void test_parse_declare_const(void) {
    const char *input = "(declare-const x (_ BitVec 8))";
    Smt2Lexer lex; smt2_lexer_init(&lex, input, strlen(input));
    SexprArena arena; sexpr_arena_init(&arena, 0);

    Sexpr *s = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s != NULL);
    ASSERT_TRUE(sexpr_is_command(s, "declare-const"));
    ASSERT_EQ_INT(s->list.count, 3);

    /* name */
    ASSERT_TRUE(sexpr_is_symbol(s->list.items[1], "x"));

    /* sort: (_ BitVec 8) */
    Sexpr *sort = s->list.items[2];
    ASSERT_EQ_INT(sort->kind, SEXPR_LIST);
    ASSERT_EQ_INT(sort->list.count, 3);

    sexpr_arena_destroy(&arena);
}

static void test_parse_bitvec_literal(void) {
    const char *input = "#xFF";
    Smt2Lexer lex; smt2_lexer_init(&lex, input, strlen(input));
    SexprArena arena; sexpr_arena_init(&arena, 0);

    Sexpr *s = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->kind, SEXPR_BITVEC);
    ASSERT_EQ_U64(s->bv.value, 255);
    ASSERT_EQ_INT(s->bv.width, 8);

    sexpr_arena_destroy(&arena);
}

static void test_parse_keyword(void) {
    const char *input = ":produce-models";
    Smt2Lexer lex; smt2_lexer_init(&lex, input, strlen(input));
    SexprArena arena; sexpr_arena_init(&arena, 0);

    Sexpr *s = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->kind, SEXPR_KEYWORD);
    ASSERT_TRUE(sexpr_is_keyword(s, ":produce-models"));

    sexpr_arena_destroy(&arena);
}

static void test_parse_multiple(void) {
    /* Parse two consecutive top-level S-expressions */
    const char *input = "(set-logic QF_BV) (exit)";
    Smt2Lexer lex; smt2_lexer_init(&lex, input, strlen(input));
    SexprArena arena; sexpr_arena_init(&arena, 0);

    Sexpr *s1 = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s1 != NULL);
    ASSERT_TRUE(sexpr_is_command(s1, "set-logic"));

    Sexpr *s2 = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s2 != NULL);
    ASSERT_TRUE(sexpr_is_command(s2, "exit"));

    Sexpr *s3 = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s3 == NULL); /* EOF */

    sexpr_arena_destroy(&arena);
}

static void test_parse_empty_list(void) {
    const char *input = "()";
    Smt2Lexer lex; smt2_lexer_init(&lex, input, strlen(input));
    SexprArena arena; sexpr_arena_init(&arena, 0);

    Sexpr *s = sexpr_parse(&lex, &arena);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT(s->kind, SEXPR_LIST);
    ASSERT_EQ_INT(s->list.count, 0);

    sexpr_arena_destroy(&arena);
}

static void test_arena_reset(void) {
    SexprArena arena;
    sexpr_arena_init(&arena, 64);

    void *p1 = sexpr_arena_alloc(&arena, 16, 8);
    ASSERT_TRUE(p1 != NULL);

    sexpr_arena_reset(&arena);

    void *p2 = sexpr_arena_alloc(&arena, 16, 8);
    ASSERT_TRUE(p2 != NULL);
    /* After reset, allocation reuses the same block */
    ASSERT_TRUE(p2 == p1);

    sexpr_arena_destroy(&arena);
}

/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_smt2_parser:\n");

    RUN(test_parse_atom_symbol);
    RUN(test_parse_atom_numeral);
    RUN(test_parse_simple_list);
    RUN(test_parse_nested);
    RUN(test_parse_indexed);
    RUN(test_parse_bv_literal_indexed);
    RUN(test_parse_declare_const);
    RUN(test_parse_bitvec_literal);
    RUN(test_parse_keyword);
    RUN(test_parse_multiple);
    RUN(test_parse_empty_list);
    RUN(test_arena_reset);

    printf("\n%d passed, %d failed\n", _passed, _failed);
    return _failed > 0 ? 1 : 0;
}
