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

static int lingering(int *lsn, int secs) {
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
    struct linger lg = { 1, secs };
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
    int lsn, c = lingering(&lsn, 2);
    double t0 = now();
    close(c);
    assert(now() - t0 > 1.5);
    close(lsn);
    printf("test_fdclose control: plain close blocked %.1f s\n", now() - t0);

    c = lingering(&lsn, 2);
    t0 = now();
    fd_close_detached(&c, 1);
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
    int on = 0;
    if (setsockopt(r, SOL_SOCKET, SO_PASSRIGHTS, &on, sizeof on) == 0) {
        on = 1;
        assert(setsockopt(r, SOL_SOCKET, SO_PASSRIGHTS, &on, sizeof on) == 0);
        fdclose_sock = r;
        pid_t big[FDCLOSE_MAX_INFLIGHT + 1];
        for (int i = 0; i < FDCLOSE_MAX_INFLIGHT; i++) big[i] = 1000000 + i;
        fdclose_pids = big;
        fdclose_n = FDCLOSE_MAX_INFLIGHT;
        fdclose_cap = FDCLOSE_MAX_INFLIGHT + 1;
        int k = open("/dev/null", O_RDONLY);
        int k2 = dup(k);
        fd_close_detached(&k2, 1);
        assert(fdclose_shut == 1 && fdclose_n == FDCLOSE_MAX_INFLIGHT + 1);
        pid_t real = big[FDCLOSE_MAX_INFLIGHT];
        assert(waitpid(real, NULL, 0) == real && fdclose_reaped(real) == 1);
        assert(fcntl(k2, F_GETFD) < 0);
        assert(fdclose_shut == 1);
        assert(send_fd(path, k) == EPERM);
        assert(fdclose_reaped(1000000) == 1 && fdclose_shut == 0);
        assert(send_fd(path, k) == 0);
        close(k);
        fdclose_sock = -1;
        fdclose_n = 0;
        fdclose_pids = NULL;
        fdclose_cap = 0;
        printf("test_fdclose cap: over the cap still closes, fd passing refused until a closer exits\n");
    } else {
        printf("test_fdclose cap: SKIP (no SO_PASSRIGHTS)\n");
    }
    close(r);
    unlink(path);
    rmdir(dir);

    c = lingering(&lsn, 30);
    fd_close_detached(&c, 1);
    usleep(300000);
    assert(fdclose_n == 1);
    t0 = now();
    fdclose_kill_all();
    assert(fdclose_n == 0 && now() - t0 < 1.0);
    close(lsn);
    printf("test_fdclose kill_all: stuck closer killed and reaped\n");

    printf("test_fdclose: OK\n");
    return 0;
}
