#ifndef FDCLOSE_H
#define FDCLOSE_H

#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SO_PASSRIGHTS
#define SO_PASSRIGHTS 83
#endif

#define FDCLOSE_MAX_INFLIGHT 32

static pid_t *fdclose_pids;
static int fdclose_n, fdclose_cap;
static int fdclose_sock = -1;
static int fdclose_shut;

static inline void fdclose_passrights(int on) {
    if (fdclose_sock < 0 || fdclose_shut == !on) return;
    if (setsockopt(fdclose_sock, SOL_SOCKET, SO_PASSRIGHTS, &on, sizeof on) == 0)
        fdclose_shut = !on;
}

static inline void fd_close_detached(const int *fds, int n) {
    if (n <= 0) return;
    if (fdclose_n >= FDCLOSE_MAX_INFLIGHT) fdclose_passrights(0);
    long pid = syscall(SYS_clone, CLONE_FILES | SIGCHLD, NULL, NULL, NULL, NULL);
    if (pid == 0) {
        for (int i = 0; i < n; i++) close(fds[i]);
        _exit(0);
    }
    if (pid < 0) {
        for (int i = 0; i < n; i++) close(fds[i]);
        return;
    }
    if (fdclose_n == fdclose_cap) {
        int nc = fdclose_cap ? fdclose_cap * 2 : FDCLOSE_MAX_INFLIGHT;
        pid_t *np = realloc(fdclose_pids, (size_t)nc * sizeof *np);
        if (!np) return;
        fdclose_pids = np;
        fdclose_cap = nc;
    }
    fdclose_pids[fdclose_n++] = (pid_t)pid;
}

static inline int fdclose_reaped(pid_t pid) {
    for (int i = 0; i < fdclose_n; i++) {
        if (fdclose_pids[i] != pid) continue;
        fdclose_pids[i] = fdclose_pids[--fdclose_n];
        if (fdclose_n < FDCLOSE_MAX_INFLIGHT) fdclose_passrights(1);
        return 1;
    }
    return 0;
}

static inline void fdclose_kill_all(void) {
    for (int i = 0; i < fdclose_n; i++) kill(fdclose_pids[i], SIGKILL);
    struct timespec tick = { 0, 10000000 };
    for (int t = 0; t < 100 && fdclose_n; t++) {
        for (int i = 0; i < fdclose_n; ) {
            if (waitpid(fdclose_pids[i], NULL, WNOHANG) != 0) fdclose_pids[i] = fdclose_pids[--fdclose_n];
            else i++;
        }
        if (fdclose_n) nanosleep(&tick, NULL);
    }
}

#endif
