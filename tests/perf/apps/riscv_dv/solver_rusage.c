/*
 * solver_rusage: VERILATOR_SOLVER wrapper for timed riscv-dv runs.
 *
 *   VERILATOR_SOLVER="solver_rusage <solver> <args...>"
 *   DVS_RUSAGE_OUT=<file>   appended: one "spawn" line, then one "done" line
 *
 * The solver inherits stdin and stdout directly, so this process is not in
 * the data path (unlike solver_tap, which costs dv-solve 13% of its wall
 * time). It only waits for the solver and records its rusage.
 *
 * Verilator may SIGKILL its solver process at exit. If that kills this
 * wrapper first, the solver is reparented to the cell runner (run.py makes
 * itself a child subreaper) and the runner reaps it by the pid from the
 * "spawn" line.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t g_child;

static void on_term(int sig) {
    if (g_child > 0) kill(g_child, sig);
}

static void append(const char *path, const char *line) {
    if (!path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    ssize_t n = write(fd, line, strlen(line));
    (void)n;
    close(fd);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: solver_rusage <solver> [args...]\n");
        return 2;
    }
    const char *out = getenv("DVS_RUSAGE_OUT");
    g_child = fork();
    if (g_child < 0) { perror("solver_rusage: fork"); return 2; }
    if (g_child == 0) {
        execvp(argv[1], argv + 1);
        perror("solver_rusage: execvp");
        _exit(127);
    }
    /* Drop our copies of the pipe ends: a solver that dies must look dead to
     * Verilator (EOF on its read end), not be kept open by us. */
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); close(nul); }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    char line[512];
    snprintf(line, sizeof line, "{\"event\":\"spawn\",\"wrapper\":%d,\"pid\":%d}\n",
             (int)getpid(), (int)g_child);
    append(out, line);

    int status = 0;
    struct rusage ru;
    while (wait4(g_child, &status, 0, &ru) < 0)
        if (errno != EINTR) { perror("solver_rusage: wait4"); return 2; }
    snprintf(line, sizeof line,
             "{\"event\":\"done\",\"pid\":%d,\"user_s\":%.6f,\"sys_s\":%.6f,"
             "\"maxrss_kb\":%ld,\"status\":%d}\n",
             (int)g_child, ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6,
             ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6, ru.ru_maxrss, status);
    append(out, line);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
