/* schema-udev — native uevent -> schema rule -> action daemon (Phase 1).
 * Listens on the kernel uevent netlink (group 1), alongside systemd-udevd. */
#include "schema-udev.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#define DEV_DIR "/etc/schema-init/dev"

static int netlink_open(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof one);
    int rcv = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcv, sizeof rcv);
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;   /* kernel uevents only; NEVER group 2 (udev/libudev) */
    sa.nl_pid    = 0;   /* kernel auto-assigns a unique pid */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}

/* Receive one datagram, verifying it came from the kernel.
 * Returns length (>=0) on a verified event; -1 to skip (spoofed / would-block /
 * ENOBUFS — check errno: EAGAIN/EWOULDBLOCK means drain complete). */
static ssize_t netlink_recv(int fd, char *buf, size_t bufsz) {
    struct iovec iov = { buf, bufsz };
    struct sockaddr_nl sa;
    char cbuf[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr msg = { &sa, sizeof sa, &iov, 1, cbuf, sizeof cbuf, 0 };
    ssize_t n = recvmsg(fd, &msg, 0);
    if (n < 0) return -1;                 /* errno set by recvmsg */
    if (sa.nl_pid != 0) { errno = 0; return -1; }   /* not from kernel */
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (!cm || cm->cmsg_type != SCM_CREDENTIALS) { errno = 0; return -1; }
    struct ucred *uc = (struct ucred *)CMSG_DATA(cm);
    if (uc->uid != 0) { errno = 0; return -1; }     /* not root/kernel */
    return n;
}

int main(void) {
    int nlfd = netlink_open();
    if (nlfd < 0) {
        fprintf(stderr, "[schema-udev] netlink open/bind failed: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "[schema-udev] listening on kernel uevent netlink (group 1)\n");

    for (;;) {
        struct pollfd pfd = { nlfd, POLLIN, 0 };
        if (poll(&pfd, 1, -1) < 0) { if (errno == EINTR) continue; break; }
        for (;;) {
            char buf[8192];
            ssize_t n = netlink_recv(nlfd, buf, sizeof buf);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;  /* drained */
                if (errno == ENOBUFS) {
                    fprintf(stderr, "[schema-udev] kernel dropped events (ENOBUFS)\n");
                    continue;
                }
                if (errno == EINTR) continue;
                if (errno == 0) continue;   /* spoofed datagram skipped */
                break;                      /* real error */
            }
            struct uevent ev;
            if (uevent_parse(buf, (size_t)n, &ev) != 0) continue;
            /* Task 5 replaces this log with rule matching + hook dispatch. */
            fprintf(stderr, "[schema-udev] uevent %s %s\n",
                    uevent_get(&ev, "ACTION"),
                    uevent_get(&ev, "DEVPATH") ? uevent_get(&ev, "DEVPATH") : "?");
        }
    }
    close(nlfd);
    return 0;
}
