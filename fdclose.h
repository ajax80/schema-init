#ifndef FDCLOSE_H
#define FDCLOSE_H

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* PID 1 hands every fd someone else sent it to a closer child. close() of one
 * can block as long as its maker likes (FUSE flush, TCP linger, even a pipe
 * whose writer faults on a FUSE mmap), and any local user can send one to the
 * 0666 notify socket. A CLONE_FILES child shares PID 1's fd table, so its
 * close() removes the fd for PID 1 too and only the child waits. Do not add
 * an inline fast path for "safe" fd types: there is none (#390). The only
 * inline closes are last resorts when no closer can be made. */

/* Linux 6.16+. Older headers lack it; setsockopt then just fails. */
#ifndef SO_PASSRIGHTS
#define SO_PASSRIGHTS 83
#endif

/* At the cap, fd passing on the notify socket is shut until a closer exits;
 * where SO_PASSRIGHTS is missing, notify reads pause instead. A soft bound:
 * fds already queued, up to NOTIFY_MAX_FDS a message, each still get one. */
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

static inline int fdclose_full(void) {
    if (fdclose_n < FDCLOSE_MAX_INFLIGHT) return 0;
    fdclose_passrights(0);
    return !fdclose_shut;
}

static inline void fdclose_spawn(int fd) {
    fdclose_full();
    long pid = syscall(SYS_clone, CLONE_FILES | SIGCHLD, NULL, NULL, NULL, NULL);
    if (pid == 0) {
        close(fd);
        _exit(0);
    }
    if (pid < 0) {
        /* no closer possible: last-resort inline close, which can block */
        close(fd);
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

static inline void fd_close_detached(const int *fds, int n) {
    for (int i = 0; i < n; i++) fdclose_spawn(fds[i]);
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

/* close() drops the fd from the shared table before it blocks, so a closer
 * already inside close() has done its job for PID 1 and is safe to kill. */
static inline int fdclose_in_close(pid_t pid) {
    char p[64], line[64];
    snprintf(p, sizeof p, "/proc/%d/syscall", (int)pid);
    FILE *f = fopen(p, "re");
    if (!f) return 0;
    long nr = -1;
    if (fgets(line, sizeof line, f)) sscanf(line, "%ld", &nr);
    fclose(f);
    return nr == SYS_close;
}

/* Before re-exec: no closer may outlive the old image still sharing its fd
 * table, and the new image cannot track them. Wait until each has exited or
 * reached close(), then kill the rest and give them 1 s to die; one stuck in
 * D state can still survive. -1 = one never reached close(); refuse the
 * re-exec rather than kill it with its fd still in PID 1's table. */
static inline int fdclose_quiesce(void) {
    struct timespec tick = { 0, 10000000 };
    for (int t = 0; t < 100; t++) {
        int pending = 0;
        for (int i = 0; i < fdclose_n; ) {
            if (waitpid(fdclose_pids[i], NULL, WNOHANG) != 0) {
                fdclose_pids[i] = fdclose_pids[--fdclose_n];
                continue;
            }
            if (!fdclose_in_close(fdclose_pids[i])) pending = 1;
            i++;
        }
        if (!pending) break;
        nanosleep(&tick, NULL);
        if (t == 99) return -1;
    }
    for (int i = 0; i < fdclose_n; i++) kill(fdclose_pids[i], SIGKILL);
    for (int t = 0; t < 100 && fdclose_n; t++) {
        for (int i = 0; i < fdclose_n; ) {
            if (waitpid(fdclose_pids[i], NULL, WNOHANG) != 0) fdclose_pids[i] = fdclose_pids[--fdclose_n];
            else i++;
        }
        if (fdclose_n) nanosleep(&tick, NULL);
    }
    return 0;
}

#endif
