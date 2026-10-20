#ifndef FDCLOSE_H
#define FDCLOSE_H

#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SO_PASSRIGHTS
#define SO_PASSRIGHTS 83
#endif

#define FDCLOSE_MAX_INFLIGHT 32

/* close() of an fd someone else made can block for as long as they like: a
 * FUSE flush waits on the FUSE daemon, a TCP socket waits out its linger.
 * A closer child shares the caller's fd table (CLONE_FILES), so its close()
 * removes the fds for the caller too and only the child waits. */
static pid_t fdclose_pids[FDCLOSE_MAX_INFLIGHT];
static int fdclose_n;
static int fdclose_sock = -1;      /* fd-passing socket to shut while closers are stuck */
static int fdclose_shut;

static inline void fdclose_passrights(int on) {
    if (fdclose_sock < 0 || fdclose_shut == !on) return;
    if (setsockopt(fdclose_sock, SOL_SOCKET, SO_PASSRIGHTS, &on, sizeof on) == 0)
        fdclose_shut = !on;
}

/* Returns 1 when a child closes them, 0 when the caller closed them itself
 * (no child could be made, or the cap is hit and fd passing cannot be shut),
 * -1 when the cap is hit and they stay open (fd passing is now refused, so
 * no more can arrive). */
static inline int fd_close_detached(const int *fds, int n) {
    if (n <= 0) return 1;
    if (fdclose_n >= FDCLOSE_MAX_INFLIGHT) {
        fdclose_passrights(0);
        if (fdclose_shut) return -1;
        for (int i = 0; i < n; i++) close(fds[i]);
        return 0;
    }
    long pid = syscall(SYS_clone, CLONE_FILES | SIGCHLD, NULL, NULL, NULL, NULL);
    if (pid == 0) {
        for (int i = 0; i < n; i++) close(fds[i]);
        _exit(0);
    }
    if (pid < 0) {
        for (int i = 0; i < n; i++) close(fds[i]);
        return 0;
    }
    fdclose_pids[fdclose_n++] = (pid_t)pid;
    return 1;
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

#endif
