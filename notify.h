#ifndef NOTIFY_H
#define NOTIFY_H

/* sd_notify readiness protocol, PID 1 side. A service with notify=1 gets
 * NOTIFY_SOCKET pointing at a datagram socket PID 1 owns; the daemon sends
 * newline-separated KEY=VALUE assignments (READY=1, STATUS=..., ...). The
 * sender is identified by kernel-attested credentials (SO_PASSCRED), and a
 * message counts only for the service whose cgroup holds that pid. */

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NOTIFY_SOCK_PATH "/run/schema-init/notify"
#define NOTIFY_MSG_MAX   4096
#define NOTIFY_STATUS_MAX 128

struct notify_msg {
    int  ready;
    int  reloading;
    int  stopping;
    int  watchdog;
    char status[NOTIFY_STATUS_MAX];
};

/* Parse one datagram. Unknown keys are ignored, as sd_notify expects.
 * Returns 1 if anything this side acts on was present, else 0. */
static inline int notify_parse(const char *buf, size_t len, struct notify_msg *m) {
    memset(m, 0, sizeof *m);
    int any = 0;
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (l == 7 && !memcmp(p, "READY=1", 7))          { m->ready = 1; any = 1; }
        else if (l == 11 && !memcmp(p, "RELOADING=1", 11)) { m->reloading = 1; any = 1; }
        else if (l == 10 && !memcmp(p, "STOPPING=1", 10))  { m->stopping = 1; any = 1; }
        else if (l == 10 && !memcmp(p, "WATCHDOG=1", 10))  { m->watchdog = 1; any = 1; }
        else if (l > 7 && !memcmp(p, "STATUS=", 7)) {
            size_t sl = l - 7;
            if (sl >= NOTIFY_STATUS_MAX) sl = NOTIFY_STATUS_MAX - 1;
            memcpy(m->status, p + 7, sl);
            m->status[sl] = '\0';
            for (char *c = m->status; *c; c++)
                if ((unsigned char)*c < 0x20) *c = ' ';
            any = 1;
        }
        p += l + 1;
    }
    return any;
}

/* Does the "0::<path>" line of a /proc/<pid>/cgroup text place the process
 * inside CG_PATH (a /sys/fs/cgroup/... directory) or a child of it? */
static inline int notify_cgroup_text_matches(const char *text, const char *cg_path) {
    const char *root = "/sys/fs/cgroup";
    size_t rl = strlen(root);
    if (strncmp(cg_path, root, rl) != 0) return 0;
    const char *want = cg_path + rl;               /* "/schema-init/<svc>" */
    size_t wl = strlen(want);
    if (wl == 0) return 0;
    const char *line = strstr(text, "0::");
    if (!line || (line != text && line[-1] != '\n')) return 0;
    const char *have = line + 3;
    size_t hl = strcspn(have, "\n");
    if (hl < wl || memcmp(have, want, wl) != 0) return 0;
    return hl == wl || have[wl] == '/';
}

static inline int notify_pid_in_cgroup(pid_t pid, const char *cg_path) {
    if (!cg_path || !cg_path[0]) return 0;
    char path[64], text[512];
    snprintf(path, sizeof path, "/proc/%d/cgroup", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, text, sizeof text - 1);
    close(fd);
    if (n <= 0) return 0;
    text[n] = '\0';
    return notify_cgroup_text_matches(text, cg_path);
}

/* Anyone may send (daemons drop to their own uids); attribution is by the
 * kernel-attested pid, checked against the service's cgroup. */
static inline int notify_open(const char *path) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof one) != 0) { close(fd); return -1; }
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) { close(fd); return -1; }
    strcpy(a.sun_path, path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    chmod(path, 0666);
    return fd;
}

/* Receive one datagram. Returns its length (NUL-terminated in buf) and the
 * sender pid, 0 when drained, -1 for a message to discard: no credentials,
 * truncated, or carrying fds (FDSTORE is not supported; they are closed). */
static inline ssize_t notify_recv(int fd, char *buf, size_t bufsz, pid_t *pid) {
    struct iovec iov = { buf, bufsz - 1 };
    union {
        char b[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(sizeof(int) * 16)];
        struct cmsghdr align;
    } ctl;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl.b;
    mh.msg_controllen = sizeof ctl.b;
    ssize_t n = recvmsg(fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    int have_cred = 0, bad = (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET) continue;
        if (c->cmsg_type == SCM_CREDENTIALS && c->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
            struct ucred uc;
            memcpy(&uc, CMSG_DATA(c), sizeof uc);
            *pid = uc.pid;
            have_cred = 1;
        } else if (c->cmsg_type == SCM_RIGHTS) {
            int nfd = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < nfd; i++) {
                int x;
                memcpy(&x, CMSG_DATA(c) + i * sizeof(int), sizeof x);
                close(x);
            }
            bad = 1;
        }
    }
    if (!have_cred || bad || *pid <= 0) return -1;
    buf[n] = '\0';
    return n > 0 ? n : -1;
}

#endif /* NOTIFY_H */
