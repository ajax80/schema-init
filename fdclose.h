#ifndef FDCLOSE_H
#define FDCLOSE_H

#include <sched.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
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

/* At most MAX_INFLIGHT closers with one fd each, plus one overflow closer.
 * At the cap, fd passing on the notify socket is shut until a closer exits.
 * Fds that still arrive (queued before the shut, or no SO_PASSRIGHTS before
 * 6.16) park here, and the overflow closer takes the whole parked list at
 * once. Its memory is a copy taken at clone, so PID 1 empties its own list. */
#define FDCLOSE_MAX_INFLIGHT 32
#define FDCLOSE_PARK_MAX     4096   /* PID 1's hard NOFILE: room for any batch */
#define FDCLOSE_PARK_PAUSE   1024   /* notify reads stop past this many parked */
#define FDCLOSE_MSG_FDS      253    /* SCM_MAX_FD: most one message can carry */

static pid_t fdclose_pids[FDCLOSE_MAX_INFLIGHT + 1];
static pid_t fdclose_from_pid[FDCLOSE_MAX_INFLIGHT + 1];
static uid_t fdclose_from_uid[FDCLOSE_MAX_INFLIGHT + 1];
static int fdclose_n;
static pid_t fdclose_ovf;
static int fdclose_parked[FDCLOSE_PARK_MAX];
static int fdclose_nparked;
static pid_t fdclose_park_pid;
static uid_t fdclose_park_uid;
static int fdclose_ovf_fds[FDCLOSE_PARK_MAX];
static int fdclose_novf;
static int fdclose_sock = -1;
static int fdclose_shut;
static pid_t fdclose_sender_pid;
static uid_t fdclose_sender_uid = (uid_t)-1;

static inline int fdclose_perfd(void) {
    return fdclose_n - (fdclose_ovf != 0);
}

static inline void fdclose_passrights(int on) {
    if (fdclose_sock < 0 || fdclose_shut == !on) return;
    if (setsockopt(fdclose_sock, SOL_SOCKET, SO_PASSRIGHTS, &on, sizeof on) == 0)
        fdclose_shut = !on;
}

/* 1 = the caller must stop reading the notify socket: the next message could
 * carry more fds than are left to park. */
static inline int fdclose_full(void) {
    if (fdclose_perfd() < FDCLOSE_MAX_INFLIGHT) return 0;
    fdclose_passrights(0);
    return fdclose_nparked > FDCLOSE_PARK_PAUSE - FDCLOSE_MSG_FDS;
}

static inline void fdclose_track(pid_t pid) {
    fdclose_pids[fdclose_n] = pid;
    fdclose_from_pid[fdclose_n] = fdclose_sender_pid;
    fdclose_from_uid[fdclose_n] = fdclose_sender_uid;
    fdclose_n++;
}

static inline void fdclose_untrack(int i) {
    if (fdclose_pids[i] == fdclose_ovf) fdclose_ovf = 0;
    fdclose_n--;
    fdclose_pids[i] = fdclose_pids[fdclose_n];
    fdclose_from_pid[i] = fdclose_from_pid[fdclose_n];
    fdclose_from_uid[i] = fdclose_from_uid[fdclose_n];
}

static inline void fdclose_flush(void) {
    if (fdclose_ovf || !fdclose_nparked) return;
    long pid = syscall(SYS_clone, CLONE_FILES | SIGCHLD, NULL, NULL, NULL, NULL);
    if (pid == 0) {
        for (int i = 0; i < fdclose_nparked; i++) close(fdclose_parked[i]);
        _exit(0);
    }
    if (pid < 0) return;
    pid_t sp = fdclose_sender_pid;
    uid_t su = fdclose_sender_uid;
    fdclose_sender_pid = fdclose_park_pid;
    fdclose_sender_uid = fdclose_park_uid;
    fdclose_ovf = (pid_t)pid;
    fdclose_track(fdclose_ovf);
    fdclose_sender_pid = sp;
    fdclose_sender_uid = su;
    memcpy(fdclose_ovf_fds, fdclose_parked, (size_t)fdclose_nparked * sizeof *fdclose_parked);
    fdclose_novf = fdclose_nparked;
    fdclose_nparked = 0;
}

/* Received fds are CLOEXEC. At re-exec, execve would close the parked ones,
 * and any the overflow closer has not reached, inside PID 1 itself. Kept open
 * across the exec, the new image closes them as strays through closers. */
static inline void fdclose_set_cloexec(int on) {
    for (int i = 0; i < fdclose_nparked + fdclose_novf; i++) {
        int fd = i < fdclose_nparked ? fdclose_parked[i] : fdclose_ovf_fds[i - fdclose_nparked];
        int fl = fcntl(fd, F_GETFD);
        if (fl >= 0) fcntl(fd, F_SETFD, on ? (fl | FD_CLOEXEC) : (fl & ~FD_CLOEXEC));
    }
}

static inline void fdclose_spawn(int fd) {
    if (fdclose_perfd() >= FDCLOSE_MAX_INFLIGHT) {
        fdclose_full();
        if (fdclose_nparked < FDCLOSE_PARK_MAX) {
            if (!fdclose_nparked) {
                fdclose_park_pid = fdclose_sender_pid;
                fdclose_park_uid = fdclose_sender_uid;
            }
            fdclose_parked[fdclose_nparked++] = fd;
            return;
        }
        /* no room to park: last-resort inline close, which can block */
        close(fd);
        return;
    }
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
    fdclose_track((pid_t)pid);
}

static inline void fd_close_detached(const int *fds, int n) {
    for (int i = 0; i < n; i++) fdclose_spawn(fds[i]);
    fdclose_flush();
}

static inline int fdclose_reaped(pid_t pid) {
    for (int i = 0; i < fdclose_n; i++) {
        if (fdclose_pids[i] != pid) continue;
        if (pid == fdclose_ovf) fdclose_novf = 0;
        fdclose_untrack(i);
        fdclose_flush();
        if (fdclose_perfd() < FDCLOSE_MAX_INFLIGHT) fdclose_passrights(1);
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
 * D state can still survive. Fds an overflow closer had not reached, and any
 * still parked, stay in the table: the new image closes them as strays. -1 = one never reached close(); refuse the
 * re-exec rather than kill it with its fd still in PID 1's table. */
static inline int fdclose_quiesce(void) {
    struct timespec tick = { 0, 10000000 };
    for (int t = 0; t < 100; t++) {
        int pending = 0;
        for (int i = 0; i < fdclose_n; ) {
            if (waitpid(fdclose_pids[i], NULL, WNOHANG) != 0) {
                fdclose_untrack(i);
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
            if (waitpid(fdclose_pids[i], NULL, WNOHANG) != 0) fdclose_untrack(i);
            else i++;
        }
        if (fdclose_n) nanosleep(&tick, NULL);
    }
    return 0;
}

#endif
