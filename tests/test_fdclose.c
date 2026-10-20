#include "../fdclose.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* A TCP socket whose peer never reads, send queue full, SO_LINGER 2 s:
 * close() blocks until the linger runs out. */
static int lingering(int *lsn) {
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    *lsn = socket(AF_INET, SOCK_STREAM, 0);
    int small = 4096;
    assert(setsockopt(*lsn, SOL_SOCKET, SO_RCVBUF, &small, sizeof small) == 0);
    assert(bind(*lsn, (struct sockaddr *)&a, sizeof a) == 0 && listen(*lsn, 1) == 0);
    assert(getsockname(*lsn, (struct sockaddr *)&a, &al) == 0);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    assert(setsockopt(c, SOL_SOCKET, SO_SNDBUF, &small, sizeof small) == 0);
    assert(connect(c, (struct sockaddr *)&a, sizeof a) == 0);
    fcntl(c, F_SETFL, O_NONBLOCK);
    char junk[4096];
    memset(junk, 'x', sizeof junk);
    while (write(c, junk, sizeof junk) > 0) {}
    assert(errno == EAGAIN);
    fcntl(c, F_SETFL, 0);
    struct linger lg = { 1, 2 };
    assert(setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof lg) == 0);
    return c;
}

static int send_fd(const char *path, int fd) {
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    struct iovec iov = { "X=1", 3 };
    union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr al; } ctl;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_name = &a;
    mh.msg_namelen = sizeof a;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl.b;
    mh.msg_controllen = sizeof ctl.b;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    int rc = sendmsg(s, &mh, 0) == 3 ? 0 : errno;
    close(s);
    return rc;
}

int main(void) {
    int lsn, c = lingering(&lsn);
    double t0 = now();
    close(c);
    assert(now() - t0 > 1.5);
    close(lsn);
    printf("test_fdclose control: plain close blocked %.1f s\n", now() - t0);

    c = lingering(&lsn);
    t0 = now();
    assert(fd_close_detached(&c, 1) == 1);
    assert(now() - t0 < 0.5);
    assert(fdclose_n == 1);
    usleep(300000);
    assert(fcntl(c, F_GETFD) < 0 && errno == EBADF);
    int st;
    pid_t p = waitpid(-1, &st, WNOHANG);
    assert(p == 0);
    p = waitpid(-1, &st, 0);
    assert(p == fdclose_pids[0] && WIFEXITED(st) && now() - t0 > 1.5);
    assert(fdclose_reaped(p) == 1 && fdclose_n == 0);
    assert(fdclose_reaped(p) == 0);
    close(lsn);
    printf("test_fdclose detached: caller returned at once, child waited the linger\n");

    char dir[] = "/tmp/schema-fdclose-XXXXXX";
    assert(mkdtemp(dir));
    char path[128];
    snprintf(path, sizeof path, "%s/sock", dir);
    int r = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    assert(bind(r, (struct sockaddr *)&a, sizeof a) == 0);
    fdclose_sock = r;
    for (int i = 0; i < FDCLOSE_MAX_INFLIGHT; i++) fdclose_pids[fdclose_n++] = 1000000 + i;
    int k = open("/dev/null", O_RDONLY);
    assert(fd_close_detached(&k, 1) == -1);
    assert(fcntl(k, F_GETFD) >= 0 && fdclose_shut == 1);
    assert(send_fd(path, k) == EPERM);
    assert(fdclose_reaped(1000000) == 1 && fdclose_shut == 0);
    assert(send_fd(path, k) == 0);
    close(k);
    fdclose_sock = -1;
    fdclose_n = 0;
    close(r);
    unlink(path);
    rmdir(dir);
    printf("test_fdclose cap: fd passing refused while closers are stuck, back after one exits\n");

    printf("test_fdclose: OK\n");
    return 0;
}
