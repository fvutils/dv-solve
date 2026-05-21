/*
 * dv-solve-smt2 -- SMT-LIB2 frontend for the dv-solve C solver.
 *
 * Usage: dv-solve-smt2 [options] [file.smt2]
 *
 * If no file is given, reads from stdin.
 * Options:
 *   --help     Show usage
 *   --version  Print version
 *   --stats    Print solve statistics to stderr
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smt2/smt2_lexer.h"
#include "smt2/smt2_parser.h"
#include "smt2/smt2_frontend.h"

#define VERSION_STRING "dv-solve-smt2 0.1.0"

static void _usage(FILE *f) {
    fprintf(f,
        "Usage: dv-solve-smt2 [options] [file.smt2]\n"
        "\n"
        "Options:\n"
        "  --help     Show this help\n"
        "  --version  Print version\n"
        "  --stats    Print statistics to stderr\n"
        "\n"
        "If no file is given, reads from stdin.\n");
}

/** Read an entire file (or stdin) into a malloc'd buffer. */
static char *_read_all(FILE *f, size_t *out_len) {
    size_t cap  = 4096;
    size_t used = 0;
    char  *buf  = (char *)malloc(cap);
    if (!buf) return NULL;

    for (;;) {
        size_t n = fread(buf + used, 1, cap - used, f);
        used += n;
        if (n == 0) break;
        if (used == cap) {
            cap *= 2;
            char *tmp = (char *)realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
    }
    *out_len = used;
    return buf;
}

int main(int argc, char **argv) {
    const char *input_file = NULL;
    int         show_stats = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            _usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", VERSION_STRING);
            return 0;
        }
        if (strcmp(argv[i], "--stats") == 0) {
            show_stats = 1;
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            _usage(stderr);
            return 2;
        }
        input_file = argv[i];
    }

    /* Open input */
    FILE *f;
    if (input_file) {
        f = fopen(input_file, "r");
        if (!f) {
            fprintf(stderr, "error: cannot open '%s'\n", input_file);
            return 2;
        }
    } else {
        f = stdin;
    }

    /* Read entire input into buffer */
    size_t buf_len;
    char *buf = _read_all(f, &buf_len);
    if (input_file) fclose(f);
    if (!buf) {
        fprintf(stderr, "error: out of memory reading input\n");
        return 2;
    }

    /* Initialise lexer, arena, and frontend */
    Smt2Lexer lex;
    smt2_lexer_init(&lex, buf, buf_len);

    SexprArena arena;
    sexpr_arena_init(&arena, 8192);

    Smt2Frontend fe;
    smt2_frontend_init(&fe, stdout, stderr);
    fe.print_stats = show_stats;

    /* Main dispatch loop */
    int exit_code = 0;
    for (;;) {
        sexpr_arena_reset(&arena);
        Sexpr *cmd = sexpr_parse(&lex, &arena);
        if (!cmd) break; /* EOF */

        int rc = smt2_frontend_dispatch(&fe, cmd);
        if (rc == 1) break;      /* (exit) */
        if (rc < 0) {
            exit_code = 2;
            break;
        }
    }

    /* Set exit code based on solve result */
    if (exit_code == 0 && fe.has_result) {
        switch (fe.last_result) {
        case SOLVE_OK:      exit_code = 0; break;
        case SOLVE_UNSAT:   exit_code = 1; break;
        case SOLVE_TIMEOUT: exit_code = 2; break;
        }
    }

    /* Clean up */
    smt2_frontend_destroy(&fe);
    sexpr_arena_destroy(&arena);
    free(buf);

    return exit_code;
}
