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
#include <time.h>
#include <unistd.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif
#include "smt2/smt2_lexer.h"
#include "smt2/smt2_parser.h"
#include "smt2/smt2_frontend.h"
#include "smt2/smt2_oracle.h"
#include "dvs_thread.h"
#include "dv_solve.h"

/* DVS_VERSION comes from src/dv_solve/__version__.py, through CMake. */
#ifndef DVS_VERSION
#define DVS_VERSION "unknown"
#endif
#define VERSION_STRING "dv-solve-smt2 " DVS_VERSION

static void _usage(FILE *f) {
    fprintf(f,
        "Usage: dv-solve-smt2 [options] [file.smt2]\n"
        "\n"
        "Options:\n"
        "  --help            Show this help\n"
        "  --version         Print version\n"
        "  --build-info      Print how the solver library was built, and its file\n"
        "  --stats           Print statistics to stderr\n"
        "  -v, --verbose     Print warnings and diagnostics to stderr\n"
        "  --interactive     Force interactive (per-command) stdin mode\n"
        "  --batch           Force batch (whole-file) stdin mode\n"
        "  --smt2            Accepted for compatibility (the only supported mode)\n"
        "  --no-incremental  Reject (push N) with N>1 (advisory; incremental works)\n"
        "  --engine=E        Force solve engine: cdcl | bitblast | auto\n"
        "  --mode=M          verilator | default. In verilator mode, check-sat-\n"
        "                    assuming is served as a randomization-diversity\n"
        "                    request (one seeded base solve) instead of a literal\n"
        "                    assumption solve. Use for VERILATOR_SOLVER.\n"
        "  --exit-codes=C    default | satcomp. satcomp exits 10 for sat, 20 for\n"
        "                    unsat and 0 for unknown, as boolector and bitwuzla do.\n"
        "  --verilator-hash=H  honor | ignore (verilator mode; default honor).\n"
        "                    Verilator follows each sat randomize() query with\n"
        "                    up to four random parity asserts to vary a\n"
        "                    deterministic solver's answer. ignore keeps the\n"
        "                    model already found (it satisfies every user\n"
        "                    constraint) and skips those asserts.\n"
        "\n"
        "If no file is given, reads from stdin.  Interactive mode is the\n"
        "default when stdin is a pipe/tty.\n"
        "\n"
        "Errors in the input are SMT-LIB responses on stdout:\n"
        "  (error \"line L column C: ...\")\n"
        "After an input error, every check-sat answers unknown, and\n"
        "(get-info :reason-unknown) names the error. Warnings and diagnostics\n"
        "are silent unless --stats, -v or DV_LOG is given. DV_LOG=<path> (a\n"
        "value containing '/' or '.') appends them to that file instead.\n"
        "\n"
        "Exit status (from the last check-sat):\n"
        "  0  sat\n"
        "  1  unsat (a proof: note that `set -e` scripts stop here)\n"
        "  2  unknown, or no check-sat was run\n"
        "  3  the z3 oracle disagreed and DV_ORACLE_ON_FAIL=abort\n"
        "  4  the input had an error (takes precedence over any answer)\n"
        "  5  usage error (bad option, unreadable file)\n"
        "  6  internal error\n"
        "With --exit-codes=satcomp, 0/1/2 become 10/20/0.\n");
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
 * Returns 1 on success, 0 on clean EOF, 2 for an s-expression still open at
 * end of input (g holds it, for the parser to report), -1 out of memory.
 * A ')' with nothing open is returned on its own, also for the parser to
 * report. pos is the 1-based line/column of the next byte to read; start
 * receives the position of g's first byte. */
typedef struct { uint32_t line, col; } SrcPos;
static int _read_one_sexpr(FILE *f, GrowBuf *g, SrcPos *pos, SrcPos *start) {
    g->len = 0;
    *start = *pos;
    int depth = 0;
    int in_string = 0;
    int in_quoted = 0;      /* inside |quoted symbol|, which may hold parens */
    int in_comment = 0;
    int started = 0;
    int c;

    while ((c = getc_unlocked(f)) != EOF) {
        if (_grow_push(g, c) < 0) return -1;
        if (c == '\n') { pos->line++; pos->col = 1; } else pos->col++;

        if (in_comment) {
            if (c == '\n') in_comment = 0;
            continue;
        }
        if (in_string) {
            if (c == '"') in_string = 0;
            continue;
        }
        if (in_quoted) {
            if (c == '|') in_quoted = 0;
            continue;
        }
        if (c == ';') { in_comment = 1; continue; }
        if (c == '"') { in_string  = 1; continue; }
        if (c == '|' && depth > 0) { in_quoted = 1; continue; }

        if (c == '(') { depth++; started = 1; }
        else if (c == ')' && depth == 0) {
            return 1;                      /* stray ')': the parser reports it */
        } else if (c == ')') {
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
            while ((c = getc_unlocked(f)) != EOF) {
                if (isspace((unsigned char)c) || c == '(' || c == ')' || c == ';') {
                    ungetc(c, f);
                    break;
                }
                if (_grow_push(g, c) < 0) return -1;
                pos->col++;
            }
            return 1;
        }
    }
    /* EOF. Only an s-expression that was opened and never closed is an error;
     * trailing whitespace or comments after the last command (every file ends
     * in a newline) are a clean end of input. */
    return started ? 2 : 0;
}

/* ------------------------------------------------------------------ */
/* Run modes                                                          */
/* ------------------------------------------------------------------ */

/* argv, for the oracle's run record. */
static int    g_argc;
static char **g_argv;

#ifndef _WIN32
static double _mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

/* Dispatch one command; with the oracle on, capture a check-sat's answer on its
 * way to stdout (unchanged) and hand the command to the oracle. */
static int _dispatch_checked(Smt2Frontend *fe, Smt2Oracle *orc, const Sexpr *cmd,
                             const char *text, size_t len) {
#ifndef _WIN32
    if (orc && smt2_oracle_is_check(cmd)) {
        char *ans = NULL;
        size_t an = 0;
        FILE *saved = fe->out;
        FILE *mem = open_memstream(&ans, &an);
        if (mem) fe->out = mem;
        double t0 = _mono_ms();
        int rc = smt2_frontend_dispatch(fe, cmd);
        double ms = _mono_ms() - t0;
        if (mem) {
            fe->out = saved;
            fclose(mem);
            if (an) fwrite(ans, 1, an, saved);
        }
        fflush(saved);
        smt2_oracle_after(orc, fe, cmd, text, len, ans, an, ms);
        free(ans);
        return rc;
    }
#endif
    int rc = smt2_frontend_dispatch(fe, cmd);
    if (orc) {
        fflush(fe->out);
        smt2_oracle_after(orc, fe, cmd, text, len, NULL, 0, 0.0);
    }
    return rc;
}

/* Where warnings and diagnostics go (plan D1): nowhere unless asked for. A
 * driver such as yosys-smtbmc merges our stderr into the response stream, so
 * nothing may be written there unrequested. Errors are not diagnostics: they
 * are `(error ...)` responses on stdout. */
static int g_verbose;
static FILE *_diag_stream(int show_stats) {
    const char *log = getenv("DV_LOG");
    if (log && *log && (strchr(log, '/') || strchr(log, '.'))) {
        FILE *lf = fopen(log, "a");
        if (lf) return lf;
    }
    if (show_stats || g_verbose || (log && *log)) return stderr;
#if defined(_WIN32)
    FILE *nf = fopen("NUL", "w");
#else
    FILE *nf = fopen("/dev/null", "w");
#endif
    return nf ? nf : stderr;
}

static void _close_diag(FILE *f) {
    if (f && f != stderr) fclose(f);
}

static int g_exit_satcomp;

/* The exit status for a finished session (plan D3, --help). */
static int _exit_status(const Smt2Frontend *fe, int internal_error) {
    if (internal_error)     return SMT2_EXIT_INTERNAL;
    if (fe->n_input_errors) return SMT2_EXIT_INPUT_ERROR;
    /* The last check-sat's answer, even if a later push, pop or assert
     * discarded its model. */
    int verdict = !fe->answered                             ? SMT2_EXIT_UNKNOWN
                : fe->answered_result == DVS_SOLVE_OK      ? SMT2_EXIT_SAT
                : fe->answered_result == DVS_SOLVE_UNSAT   ? SMT2_EXIT_UNSAT
                :                                            SMT2_EXIT_UNKNOWN;
    if (g_exit_satcomp)
        return verdict == SMT2_EXIT_SAT ? 10 : verdict == SMT2_EXIT_UNSAT ? 20 : 0;
    return verdict;
}

static int _run_batch(FILE *f, int show_stats, int verilator_mode) {
    size_t buf_len;
    char *buf = _read_all(f, &buf_len);
    if (!buf) {
        fprintf(stderr, "error: out of memory reading input\n");
        return SMT2_EXIT_INTERNAL;
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
    FILE *err_fp = _diag_stream(show_stats);
    smt2_frontend_init(&fe, stdout, err_fp);
    fe.print_stats = show_stats;
    fe.verilator_mode = verilator_mode != 0;
    fe.vlt_hash_ignore = verilator_mode == 2;

    Smt2Oracle *orc = smt2_oracle_from_env(g_argc, g_argv, verilator_mode != 0,
                                           verilator_mode == 2, err_fp);
    int failed = 0;
    for (;;) {
        sexpr_arena_reset(&arena);
        size_t at = lex.pos;
        Smt2Token first = smt2_lexer_peek(&lex);
        Sexpr *cmd = sexpr_parse(&lex, &arena);
        if (!cmd) {
            if (!lex.err_msg) break;        /* end of input */
            /* Malformed input: report it and carry on with the next command
             * (the parser has skipped past the bad one). The session is now
             * poisoned, so no verdict follows. */
            smt2_frontend_input_error(&fe, lex.err_line, lex.err_col, "%s", lex.err_msg);
            continue;
        }
        fe.cmd_line = first.line;
        fe.cmd_col  = first.col;

        int rc = _dispatch_checked(&fe, orc, cmd, buf + at, lex.pos - at);
        if (rc == 1) break;                 /* (exit) */
        /* A command error is recorded but does NOT terminate the REPL: staying
         * alive keeps the response stream in sync with the driver (an early exit
         * closes the pipe and hangs a driver blocked on read). */
        if (rc < 0) failed = 1;
    }

    int exit_code = _exit_status(&fe, 0);
    if (failed && exit_code != SMT2_EXIT_INPUT_ERROR) exit_code = g_exit_satcomp ? 0 : SMT2_EXIT_UNKNOWN;

    smt2_oracle_finish(orc);
    smt2_frontend_destroy(&fe);
    sexpr_arena_destroy(&arena);
    _close_diag(err_fp);
    free(buf);
    return exit_code;
}

static int _run_interactive(FILE *f, int show_stats, int verilator_mode) {
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
    FILE *err_fp = _diag_stream(show_stats);
    smt2_frontend_init(&fe, stdout, err_fp);
    fe.print_stats = show_stats;
    fe.verilator_mode = verilator_mode != 0;
    fe.vlt_hash_ignore = verilator_mode == 2;

    Smt2Oracle *orc = smt2_oracle_from_env(g_argc, g_argv, verilator_mode != 0,
                                           verilator_mode == 2, err_fp);
    int failed = 0, internal = 0;
    SrcPos pos = { 1, 1 };
    for (;;) {
        SrcPos start;
        int r = _read_one_sexpr(f, &cmd_buf, &pos, &start);
        if (r == 0) break;       /* clean EOF */
        if (r < 0)  { fprintf(stderr, "error: out of memory reading input\n");
                      internal = 1; break; }

        Smt2Lexer lex;
        smt2_lexer_init(&lex, cmd_buf.buf, cmd_buf.len);
        lex.line = start.line;
        lex.col  = start.col;

        sexpr_arena_reset(&arena);
        Smt2Token first = smt2_lexer_peek(&lex);
        Sexpr *cmd = sexpr_parse(&lex, &arena);
        if (!cmd) {
            if (lex.err_msg) {
                smt2_frontend_input_error(&fe, lex.err_line, lex.err_col, "%s", lex.err_msg);
                fflush(stdout);
            }
            if (r == 2) break;   /* input ended inside an s-expression */
            continue;
        }
        fe.cmd_line = first.line;
        fe.cmd_col  = first.col;

        int rc = _dispatch_checked(&fe, orc, cmd, cmd_buf.buf, cmd_buf.len);
        fflush(stdout);
        if (rc == 1) break;                 /* (exit) */
        /* Keep the REPL alive on a command error so the response stream stays
         * in sync with the driver (an early exit would hang a driver that is
         * blocked reading our next reply). */
        if (rc < 0) failed = 1;
    }

    int exit_code = _exit_status(&fe, internal);
    if (failed && exit_code != SMT2_EXIT_INPUT_ERROR && exit_code != SMT2_EXIT_INTERNAL)
        exit_code = g_exit_satcomp ? 0 : SMT2_EXIT_UNKNOWN;

    smt2_oracle_finish(orc);
    smt2_frontend_destroy(&fe);
    sexpr_arena_destroy(&arena);
    _close_diag(err_fp);
    free(cmd_buf.buf);
    return exit_code;
}

/* B12: the SMT2 expression translator recurses one C frame per nesting level
 * (~8.6 KB each), so deeply-nested input can overflow the default ~8 MB stack
 * and SIGSEGV. We run the solve on a thread with an explicit large stack (a
 * reliably-reserved region, unlike runtime setrlimit raises which Linux may
 * refuse to honour). The frontend's depth guard is sized from this same value
 * (g_smt2_translate_stack_bytes) so it bails to `unknown` just under the true
 * overflow point — crash-proof, never a wrong answer. */
#define SMT2_WORKER_STACK_BYTES  ((size_t)1 << 30)   /* 1 GiB */

typedef struct {
    FILE *f;
    int   show_stats;
    int   verilator_mode;
    int   interactive;
    int   rc;
} Smt2WorkerArgs;

static void *_smt2_worker(void *p) {
    Smt2WorkerArgs *a = (Smt2WorkerArgs *)p;
    a->rc = a->interactive
        ? _run_interactive(a->f, a->show_stats, a->verilator_mode)
        : _run_batch(a->f, a->show_stats, a->verilator_mode);
    return NULL;
}

/* Run the batch/interactive dispatch on a large-stack thread when possible;
 * fall back to running inline on the main thread if thread setup fails (the
 * frontend's RLIMIT_STACK-based guard still keeps that path crash-proof). */
static int _run_on_worker_stack(FILE *f, int show_stats, int verilator_mode,
                                int interactive) {
    Smt2WorkerArgs args = { f, show_stats, verilator_mode, interactive, 2 };
    dvs_thread_t   tid;
    if (dvs_thread_create_stack(&tid, _smt2_worker, &args,
                                SMT2_WORKER_STACK_BYTES) == 0) {
        g_smt2_translate_stack_bytes = SMT2_WORKER_STACK_BYTES;
        dvs_thread_join(&tid, NULL);
        return args.rc;
    }
    /* Fallback: run inline; guard sizes itself from RLIMIT_STACK (0 = unset). */
    g_smt2_translate_stack_bytes = 0;
    _smt2_worker(&args);
    return args.rc;
}

int main(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
    const char *input_file = NULL;
    int         show_stats = 0;
    int         force_interactive = 0;
    int         force_batch = 0;
    int         no_incremental = 0;
    int         verilator_mode = 0;
    int         hash_ignore = 0;

#if defined(_WIN32)
    /* Replies end in "\n" as on POSIX: a text-mode stdout would turn each into
     * "\r\n", which a peer parsing the SMT-LIB2 stream does not expect. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            _usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", VERSION_STRING);
            return 0;
        }
        if (strcmp(argv[i], "--build-info") == 0) {
            printf("build-info %s\nlibrary %s\n", dvs_build_info(), dvs_build_library_path());
            return 0;
        }
        if (strcmp(argv[i], "--stats") == 0) { show_stats = 1; continue; }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
            continue;
        }
        if (strncmp(argv[i], "--exit-codes=", 13) == 0) {
            const char *c = argv[i] + 13;
            if (strcmp(c, "satcomp") == 0) {
                g_exit_satcomp = 1;
            } else if (strcmp(c, "default") == 0) {
                g_exit_satcomp = 0;
            } else {
                fprintf(stderr, "error: unknown --exit-codes '%s' (expected default|satcomp)\n", c);
                return SMT2_EXIT_USAGE;
            }
            continue;
        }
        if (strcmp(argv[i], "--interactive") == 0) { force_interactive = 1; continue; }
        if (strcmp(argv[i], "--batch") == 0) { force_batch = 1; continue; }
        if (strcmp(argv[i], "--smt2") == 0) { continue; }  /* compat no-op */
        /* --mode=verilator: treat check-sat-assuming as a randomization
         * diversity request (see Smt2Frontend.verilator_mode). --mode=default
         * (or =smt) is the standard SMT-LIB behavior. */
        if (strncmp(argv[i], "--mode=", 7) == 0) {
            const char *m = argv[i] + 7;
            if (strcmp(m, "verilator") == 0) {
                verilator_mode = 1;
            } else if (strcmp(m, "default") == 0 || strcmp(m, "smt") == 0) {
                verilator_mode = 0;
            } else {
                fprintf(stderr, "error: unknown mode '%s' (expected verilator|default)\n", m);
                return SMT2_EXIT_USAGE;
            }
            continue;
        }
        if (strncmp(argv[i], "--verilator-hash=", 17) == 0) {
            const char *h = argv[i] + 17;
            if (strcmp(h, "ignore") == 0) {
                hash_ignore = 1;
            } else if (strcmp(h, "honor") == 0) {
                hash_ignore = 0;
            } else {
                fprintf(stderr, "error: unknown --verilator-hash '%s' (expected honor|ignore)\n", h);
                return SMT2_EXIT_USAGE;
            }
            continue;
        }
        if (strcmp(argv[i], "--no-incremental") == 0) {
            no_incremental = 1;
            (void)no_incremental;  /* reserved for future enforcement */
            continue;
        }
        /* --engine=cdcl|bitblast|auto — sets DV_ENGINE, which the frontend's
         * check-sat handler reads to pick the engine. cdcl must SET it rather
         * than clear it: an unset DV_ENGINE means auto, which bit-blasts
         * QF_UFBV/QF_ABV/QF_AUFBV. */
        if (strncmp(argv[i], "--engine=", 9) == 0) {
            const char *eng = argv[i] + 9;
            if (strcmp(eng, "auto") == 0) {
                unsetenv("DV_ENGINE");
            } else if (strcmp(eng, "cdcl") == 0) {
                setenv("DV_ENGINE", "cdcl", 1);
            } else if (strcmp(eng, "bitblast") == 0 || strcmp(eng, "bb") == 0) {
                setenv("DV_ENGINE", "bitblast", 1);
            } else {
                fprintf(stderr, "error: unknown engine '%s' (expected cdcl|bitblast|auto)\n", eng);
                return SMT2_EXIT_USAGE;
            }
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            _usage(stderr);
            return SMT2_EXIT_USAGE;
        }
        input_file = argv[i];
    }

    FILE *f;
    int interactive;

    if (input_file) {
        f = fopen(input_file, "r");
        if (!f) {
            fprintf(stderr, "error: cannot open '%s'\n", input_file);
            return SMT2_EXIT_USAGE;
        }
        interactive = 0;   /* file input is always batch */
    } else {
        f = stdin;
        interactive = 1;   /* stdin defaults to interactive */
    }

    if (force_batch)       interactive = 0;
    if (force_interactive) interactive = 1;

    /* 0: default, 1: verilator, 2: verilator with --verilator-hash=ignore */
    if (hash_ignore && !verilator_mode) {
        fprintf(stderr, "error: --verilator-hash applies only with --mode=verilator\n");
        return SMT2_EXIT_USAGE;
    }
    if (verilator_mode && hash_ignore) verilator_mode = 2;
    int rc = _run_on_worker_stack(f, show_stats, verilator_mode, interactive);

    if (input_file) fclose(f);
    return rc;
}
