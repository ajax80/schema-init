#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "verify_gate.h"

#define GATE_TIMEOUT_MS 30000

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* verify's exit code (0-3) with its stdout in *rep, or -1: unavailable, why says why */
static int run_verifier(char **rep, char *why, size_t wlen) {
    const char *bin = "/proc/self/exe";
#ifdef SCHEMA_VERIFY_TESTHOOKS
    const char *e = getenv("SCHEMA_VERIFY_BIN");
    if (e && e[0]) bin = e;
#endif
    size_t len = 0, cap = 4096;
    long long deadline = now_ms() + GATE_TIMEOUT_MS, left;
    int p[2], st, open_ = 1, timedout = 0;
    pid_t pid;

    *rep = malloc(cap);
    if (!*rep || pipe2(p, O_CLOEXEC) < 0) {
        snprintf(why, wlen, "%s", strerror(errno));
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        snprintf(why, wlen, "fork: %s", strerror(errno));
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDONLY);
        /* its own process group, so a timeout also kills whatever it started */
        setpgid(0, 0);
        dup2(nul, 0);
        dup2(p[1], 1);
        execl(bin, "schema-ctl", "verify", "--quiet", (char *)NULL);
        fprintf(stderr, "schema-ctl: %s: %s\n", bin, strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid);
    close(p[1]);
    while (open_ && (left = deadline - now_ms()) > 0) {
        struct pollfd pf = { .fd = p[0], .events = POLLIN };
        ssize_t n;
        if (poll(&pf, 1, (int)left) <= 0) continue;
        if (len + 1 >= cap) {
            char *nb = realloc(*rep, cap *= 2);
            if (!nb) break;
            *rep = nb;
        }
        n = read(p[0], *rep + len, cap - 1 - len);
        if (n > 0) len += (size_t)n;
        else if (n == 0 || errno != EINTR) open_ = 0;
    }
    close(p[0]);
    (*rep)[len] = '\0';
    /* stdout closed is not exited: the child gets the rest of the 30 s */
    while (waitpid(pid, &st, WNOHANG) == 0) {
        if (now_ms() >= deadline) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            timedout = 1;
            break;
        }
        poll(NULL, 0, 50);
    }
    if (timedout) snprintf(why, wlen, "timed out after %d s", GATE_TIMEOUT_MS / 1000);
    else if (WIFSIGNALED(st)) snprintf(why, wlen, "killed by signal %d, %s", WTERMSIG(st), strsignal(WTERMSIG(st)));
    else if (WEXITSTATUS(st) <= 3) return WEXITSTATUS(st);
    else if (WEXITSTATUS(st) == 127) snprintf(why, wlen, "could not run %s", bin);
    else snprintf(why, wlen, "verify exited %d", WEXITSTATUS(st));
    return -1;
}

int verify_gate(const char *action, int force, int interactive, FILE *out) {
    int rx = !strcmp(action, "reexec"), r;
    char *rep = NULL, why[256];

    if (force || (!rx && !interactive)) return 0;
    r = run_verifier(&rep, why, sizeof why);
    if (r == 1 || r == 2 || (r == 3 && rx)) {
        fputs(rep, out);
        if (r == 3)
            fprintf(out, "schema-ctl: %s refused — verification could not resolve the above; --force to override\n", action);
        else
            fprintf(out, "schema-ctl: %s refused — fix the above or run 'schema-ctl verify --accept'; --force to override\n", action);
        free(rep);
        return 1;
    }
    if (r == 3) {
        for (char *l = rep, *nl; *l; l = nl ? nl + 1 : l + strlen(l)) {
            nl = strchr(l, '\n');
            if (!strncmp(l, "[UNRESOLVED] ", 13))
                fprintf(out, "[WARN] %.*s\n", (int)(nl ? nl - l - 13 : (long)strlen(l + 13)), l + 13);
        }
        fprintf(out, "[WARN] verification unresolved; proceeding with %s\n", action);
    } else if (r < 0 && rx) {
        fprintf(out, "schema-ctl: reexec refused — verification unavailable (%s); --force to override\n", why);
        free(rep);
        return 1;
    } else if (r < 0) {
        /* never strand a box that needs to go down */
        fprintf(out, "schema-ctl: verify %s\n[WARN] verification unavailable; proceeding with %s\n", why, action);
    }
    free(rep);
    return 0;
}
