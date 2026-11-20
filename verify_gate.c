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
/* after SIGKILL: a child stuck in D state never dies, and the gate must not wait on it */
#define GATE_REAP_MS 2000

static volatile sig_atomic_t gate_sig;
static void on_sig(int s) { gate_sig = s; }

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 1 reaped, 0 still running at give_up (or a signal came, if on_signal), -1 lost (waitpid failed) */
static int reap_until(pid_t pid, int *st, long long give_up, int on_signal) {
    for (;;) {
        pid_t w = waitpid(pid, st, WNOHANG);
        if (w == pid) return 1;
        if (w < 0 && errno != EINTR) return -1;
        if ((on_signal && gate_sig) || now_ms() >= give_up) return 0;
        poll(NULL, 0, 50);
    }
}

/* verify's exit code (0-3) with its stdout+stderr in *rep, or -1: unavailable, why says why */
static int run_verifier(char **rep, char *why, size_t wlen) {
    static const int sigs[] = { SIGINT, SIGTERM, SIGHUP };
    const char *bin = "/proc/self/exe";
    long long timeout = GATE_TIMEOUT_MS;
    int nokill = 0;
#ifdef SCHEMA_VERIFY_TESTHOOKS
    const char *e = getenv("SCHEMA_VERIFY_BIN"), *t = getenv("SCHEMA_VERIFY_GATE_TIMEOUT_MS");
    if (e && e[0]) bin = e;
    if (t && t[0]) timeout = atoll(t);
    /* stands in for a child SIGKILL can't end (D state) */
    nokill = getenv("SCHEMA_VERIFY_GATE_NOKILL") != NULL;
#endif
    struct sigaction dfl = { .sa_handler = SIG_DFL }, catch = { .sa_handler = on_sig }, old_chld, old[3];
    sigset_t chld, old_mask;
    size_t len = 0, cap = 4096;
    long long deadline;
    int p[2], nul, st = 0, open_ = 1, r = -1, done;
    pid_t pid;

    *rep = calloc(1, cap);
    if (!*rep) {
        snprintf(why, wlen, "%s", strerror(errno));
        return -1;
    }
    nul = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (nul < 0 || pipe2(p, O_CLOEXEC) < 0) {
        snprintf(why, wlen, "%s", strerror(errno));
        if (nul >= 0) close(nul);
        return -1;
    }
    /* an inherited SIG_IGN has the kernel reap the verifier, and its verdict is lost */
    sigaction(SIGCHLD, &dfl, &old_chld);
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigprocmask(SIG_UNBLOCK, &chld, &old_mask);
    gate_sig = 0;
    for (int i = 0; i < 3; i++) {
        sigaction(sigs[i], NULL, &old[i]);
        if (old[i].sa_handler != SIG_IGN) sigaction(sigs[i], &catch, NULL);
    }
    pid = fork();
    if (pid == 0) {
        /* its own process group, so a timeout or ^C also kills whatever it started */
        setpgid(0, 0);
        dup2(nul, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        execl(bin, "schema-ctl", "verify", "--quiet", (char *)NULL);
        fprintf(stderr, "schema-ctl: %s: %s\n", bin, strerror(errno));
        _exit(127);
    }
    close(nul);
    close(p[1]);
    if (pid < 0) {
        snprintf(why, wlen, "fork: %s", strerror(errno));
        close(p[0]);
        goto out;
    }
    setpgid(pid, pid);
    deadline = now_ms() + timeout;
    while (open_ && !gate_sig && deadline > now_ms()) {
        struct pollfd pf = { .fd = p[0], .events = POLLIN };
        ssize_t n;
        if (poll(&pf, 1, (int)(deadline - now_ms())) <= 0) continue;
        if (len + 1 >= cap) {
            char *nb = realloc(*rep, cap * 2);
            if (!nb) break;
            *rep = nb;
            cap *= 2;
        }
        n = read(p[0], *rep + len, cap - 1 - len);
        if (n > 0) len += (size_t)n;
        else if (n == 0 || errno != EINTR) open_ = 0;
    }
    close(p[0]);
    (*rep)[len] = '\0';
    /* stdout closed is not exited: the child gets the rest of the time */
    done = reap_until(pid, &st, deadline, 1);
    if (done == 0) {
        if (!nokill) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
        }
        reap_until(pid, &st, now_ms() + GATE_REAP_MS, 0);
        snprintf(why, wlen, "timed out after %lld s", timeout / 1000);
    } else if (done < 0)
        snprintf(why, wlen, "lost track of it: waitpid: %s", strerror(errno));
    else if (WIFSIGNALED(st))
        snprintf(why, wlen, "killed by signal %d, %s", WTERMSIG(st), strsignal(WTERMSIG(st)));
    else if (WEXITSTATUS(st) <= 3)
        r = WEXITSTATUS(st);
    else if (WEXITSTATUS(st) == 127)
        snprintf(why, wlen, "could not run %s", bin);
    else
        snprintf(why, wlen, "verify exited %d", WEXITSTATUS(st));
out:
    sigaction(SIGCHLD, &old_chld, NULL);
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
    for (int i = 0; i < 3; i++)
        if (old[i].sa_handler != SIG_IGN) sigaction(sigs[i], &old[i], NULL);
    if (gate_sig > 0) {
        snprintf(why, wlen, "interrupted");
        raise(gate_sig);
        r = -1;
    }
    return r;
}

int verify_gate(const char *action, int force, int interactive, FILE *out) {
    /* action may carry reexec's path ("reexec /usr/sbin/schema-init"): the override names it */
    int rx = !strncmp(action, "reexec", 6) && (!action[6] || action[6] == ' '), r;
    char *rep = NULL, why[256];

    if (force || (!rx && !interactive)) return 0;
    r = run_verifier(&rep, why, sizeof why);
    if (r == 1 || r == 2 || (r == 3 && rx)) {
        fputs(rep, out);
        if (r == 3)
            fprintf(out, "schema-ctl: %s refused — verification could not resolve the above; "
                    "run 'schema-ctl %s --force' to override\n", action, action);
        else
            fprintf(out, "schema-ctl: %s refused — fix the above or run 'schema-ctl verify --accept'; "
                    "run 'schema-ctl %s --force' to override\n", action, action);
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
        fprintf(out, "schema-ctl: %s refused — verification unavailable (%s); run 'schema-ctl %s --force' to override\n",
                action, why, action);
        free(rep);
        return 1;
    } else if (r < 0) {
        /* never strand a box that needs to go down */
        fprintf(out, "schema-ctl: verify %s\n[WARN] verification unavailable; proceeding with %s\n", why, action);
    }
    free(rep);
    return 0;
}
