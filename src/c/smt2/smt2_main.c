/*
 * dv-solve-smt2 -- SMT-LIB2 frontend for the dv-solve C solver.
 *
 * Usage: dv-solve-smt2 [options] [file.smt2]
 *
 * If no file is given, reads from stdin in interactive (one-S-expr-at-a-time)
 * mode.  Use --batch to force batch-mode (read entire input) on stdin.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "smt2/smt2_lexer.h"
#include "smt2/smt2_parser.h"
#include "smt2/smt2_frontend.h"

#define VERSION_STRING "dv-solve-smt2 0.1.0"

static void _usage(FILE *f) {
    fprintf(f,
        "Usage: dv-solve-smt2 [options] [file.smt2]\n"
        "\n"
        "Options:\n"
        "  --help            Show this help\n"
        "  --version         Print version\n"
        "  --stats           Print statistics to stderr\n"
        "  --interactive     Force interactive (per-command) stdin mode\n"
        "  --batch           Force batch (whole-file) stdin mode\n"
        "  --smt2            Accepted for compatibility (the only supported mode)\n"
        "  --no-incremental  Reject (push N) with N>1 (advisory; incremental works)\n"
        "\n"
        "If no file is given, reads from stdin.  Interactive mode is the\n"
        "default when stdin is a pipe/tty.\n");
}

/* Read an entire stream into a malloc'd buffer (batch mode). */
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

/* ------------------------------------------------------------------ */
/* Interactive read: accumulate one balanced top-level S-expression   */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} GrowBuf;

static int _grow_push(GrowBuf *g, int c) {
    if (g->len + 1 >= g->cap) {
        size_t newcap = g->cap ? g->cap * 2 : 256;
        char *tmp = (char *)realloc(g->buf, newcap);
        if (!tmp) return -1;
        g->buf = tmp;
        g->cap = newcap;
    }
    g->buf[g->len++] = (char)c;
    return 0;
}

/* Read one complete top-level S-expression from f into g.
 * Returns 1 on success, 0 on clean EOF, -1 on error. */
static int _read_one_sexpr(FILE *f, GrowBuf *g) {
    g->len = 0;
    int depth = 0;
    int in_string = 0;
    int in_comment = 0;
    int started = 0;
    int c;

    while ((c = fgetc(f)) != EOF) {
        if (_grow_push(g, c) < 0) return -1;

        if (in_comment) {
            if (c == '\n') in_comment = 0;
            continue;
        }
        if (in_string) {
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == ';') { in_comment = 1; continue; }
        if (c == '"') { in_string  = 1; continue; }

        if (c == '(') { depth++; started = 1; }
        else if (c == ')') {
            depth--;
            if (depth == 0 && started) {
                /* Null-terminate for safety (lexer uses length, but
                 * some downstream debug printfs may stringify). */
                if (_grow_push(g, '\0') < 0) return -1;
                g->len--;
                return 1;
            }
        } else if (!isspace((unsigned char)c) && depth == 0 && !started) {
            /* Top-level atom (rare in SMT-LIB).  Consume contiguous non-
             * whitespace, then return. */
            while ((c = fgetc(f)) != EOF) {
                if (isspace((unsigned char)c) || c == '(' || c == ')' || c == ';') {
                    ungetc(c, f);
                    break;
                }
                if (_grow_push(g, c) < 0) return -1;
            }
            return 1;
        }
    }
    return g->len > 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Run modes                                                          */
/* ------------------------------------------------------------------ */

static int _run_batch(FILE *f, int show_stats) {
    size_t buf_len;
    char *buf = _read_all(f, &buf_len);
    if (!buf) {
        fprintf(stderr, "error: out of memory reading input\n");
        return 2;
    }

    Smt2Lexer lex;
    smt2_lexer_init(&lex, buf, buf_len);

    SexprArena arena;
    sexpr_arena_init(&arena, 8192);

    Smt2Frontend fe;
    /* When stdout is a pipe (e.g. driven by yosys-smtbmc whose
     * Popen has stderr=STDOUT), routing diagnostics to stderr
     * pollutes the response channel — yosys-smtbmc reads the
     * combined stream and treats any non-protocol line as an
     * "unexpected response". Redirect diagnostics to a log file
     * in that case so the SMT2 response channel stays clean. */
    FILE *err_fp = stderr;
    if (!isatty(fileno(stdout))) {
        const char *log_env = getenv("DV_LOG");
        const char *log_path = (log_env && *log_env) ? log_env : "/tmp/dv-solve.log";
        FILE *lf = fopen(log_path, "a");
        if (lf) err_fp = lf;
    }
    smt2_frontend_init(&fe, stdout, err_fp);
    fe.print_stats = show_stats;

    int exit_code = 0;
    for (;;) {
        sexpr_arena_reset(&arena);
        Sexpr *cmd = sexpr_parse(&lex, &arena);
        if (!cmd) break;

        int rc = smt2_frontend_dispatch(&fe, cmd);
        if (rc == 1) break;
        if (rc < 0) { exit_code = 2; break; }
    }

    if (exit_code == 0 && fe.has_result) {
        switch (fe.last_result) {
        case SOLVE_OK:      exit_code = 0; break;
        case SOLVE_UNSAT:   exit_code = 1; break;
        case SOLVE_TIMEOUT: exit_code = 2; break;
        }
    }

    smt2_frontend_destroy(&fe);
    sexpr_arena_destroy(&arena);
    free(buf);
    return exit_code;
}

static int _run_interactive(FILE *f, int show_stats) {
    GrowBuf cmd_buf = {0};
    SexprArena arena;
    sexpr_arena_init(&arena, 8192);

    Smt2Frontend fe;
    /* When stdout is a pipe (e.g. driven by yosys-smtbmc whose
     * Popen has stderr=STDOUT), routing diagnostics to stderr
     * pollutes the response channel — yosys-smtbmc reads the
     * combined stream and treats any non-protocol line as an
     * "unexpected response". Redirect diagnostics to a log file
     * in that case so the SMT2 response channel stays clean. */
    FILE *err_fp = stderr;
    if (!isatty(fileno(stdout))) {
        const char *log_env = getenv("DV_LOG");
        const char *log_path = (log_env && *log_env) ? log_env : "/tmp/dv-solve.log";
        FILE *lf = fopen(log_path, "a");
        if (lf) err_fp = lf;
    }
    smt2_frontend_init(&fe, stdout, err_fp);
    fe.print_stats = show_stats;

    int exit_code = 0;
    for (;;) {
        int r = _read_one_sexpr(f, &cmd_buf);
        if (r == 0) break;       /* clean EOF */
        if (r < 0)  { exit_code = 2; break; }

        Smt2Lexer lex;
        smt2_lexer_init(&lex, cmd_buf.buf, cmd_buf.len);

        sexpr_arena_reset(&arena);
        Sexpr *cmd = sexpr_parse(&lex, &arena);
        if (!cmd) continue;

        int rc = smt2_frontend_dispatch(&fe, cmd);
        fflush(stdout);
        if (rc == 1) break;
        if (rc < 0) { exit_code = 2; break; }
    }

    if (exit_code == 0 && fe.has_result) {
        switch (fe.last_result) {
        case SOLVE_OK:      exit_code = 0; break;
        case SOLVE_UNSAT:   exit_code = 1; break;
        case SOLVE_TIMEOUT: exit_code = 2; break;
        }
    }

    smt2_frontend_destroy(&fe);
    sexpr_arena_destroy(&arena);
    free(cmd_buf.buf);
    return exit_code;
}

int main(int argc, char **argv) {
    const char *input_file = NULL;
    int         show_stats = 0;
    int         force_interactive = 0;
    int         force_batch = 0;
    int         no_incremental = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            _usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", VERSION_STRING);
            return 0;
        }
        if (strcmp(argv[i], "--stats") == 0) { show_stats = 1; continue; }
        if (strcmp(argv[i], "--interactive") == 0) { force_interactive = 1; continue; }
        if (strcmp(argv[i], "--batch") == 0) { force_batch = 1; continue; }
        if (strcmp(argv[i], "--smt2") == 0) { continue; }  /* compat no-op */
        if (strcmp(argv[i], "--no-incremental") == 0) {
            no_incremental = 1;
            (void)no_incremental;  /* reserved for future enforcement */
            continue;
        }
        /* --engine=cdcl|bitblast|auto — Phase B.0. Sets DV_ENGINE so the
         * frontend's check-sat handler dispatches to the bit-blast path. */
        if (strncmp(argv[i], "--engine=", 9) == 0) {
            const char *eng = argv[i] + 9;
            if (strcmp(eng, "cdcl") == 0 || strcmp(eng, "auto") == 0) {
                unsetenv("DV_ENGINE");
            } else if (strcmp(eng, "bitblast") == 0 || strcmp(eng, "bb") == 0) {
                setenv("DV_ENGINE", "bitblast", 1);
            } else {
                fprintf(stderr, "error: unknown engine '%s' (expected cdcl|bitblast|auto)\n", eng);
                return 2;
            }
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            _usage(stderr);
            return 2;
        }
        input_file = argv[i];
    }

    FILE *f;
    int interactive;

    if (input_file) {
        f = fopen(input_file, "r");
        if (!f) {
            fprintf(stderr, "error: cannot open '%s'\n", input_file);
            return 2;
        }
        interactive = 0;   /* file input is always batch */
    } else {
        f = stdin;
        interactive = 1;   /* stdin defaults to interactive */
    }

    if (force_batch)       interactive = 0;
    if (force_interactive) interactive = 1;

    int rc = interactive ? _run_interactive(f, show_stats)
                         : _run_batch(f, show_stats);

    if (input_file) fclose(f);
    return rc;
}
