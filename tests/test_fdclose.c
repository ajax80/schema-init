#include "../fdclose.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
        for (int i = 0; i < FDCLOSE_MAX_INFLIGHT; i++) fdclose_pids[i] = 1000000 + i;
        fdclose_n = FDCLOSE_MAX_INFLIGHT;
        int k = open("/dev/null", O_RDONLY);
        int k2 = dup(k);
        fd_close_detached(&k2, 1);
        assert(fdclose_shut == 1 && fdclose_n == FDCLOSE_MAX_INFLIGHT + 1 && fdclose_ovf);
        pid_t real = fdclose_ovf;
        assert(waitpid(real, NULL, 0) == real && fdclose_reaped(real) == 1 && !fdclose_ovf);
        assert(fcntl(k2, F_GETFD) < 0);
        assert(fdclose_shut == 1);
        assert(send_fd(path, k) == EPERM);
        assert(fdclose_reaped(1000000) == 1 && fdclose_shut == 0);
        assert(send_fd(path, k) == 0);
        close(k);
        fdclose_sock = -1;
        fdclose_n = 0;
        printf("test_fdclose cap: over the cap still closes, fd passing refused until a closer exits\n");
    } else {
        printf("test_fdclose cap: SKIP (no SO_PASSRIGHTS)\n");
    }
    close(r);
    unlink(path);
    rmdir(dir);

    int pp[2];
    assert(pipe(pp) == 0);
    int mf = memfd_create("x", MFD_CLOEXEC);
    int each[] = { pp[0], pp[1], mf };
    fd_close_detached(each, 3);
    assert(fdclose_n == 3);
    for (int i = 0; i < 3; i++) {
        pid_t q = waitpid(-1, NULL, 0);
        assert(fdclose_reaped(q) == 1);
    }
    for (int i = 0; i < 3; i++) assert(fcntl(each[i], F_GETFD) < 0);
    printf("test_fdclose per-fd: pipe + memfd each get their own closer, none inline\n");

    int l2, c1 = lingering(&lsn, 30), c2 = lingering(&l2, 30);
    int two[] = { c1, c2 };
    t0 = now();
    fd_close_detached(two, 2);
    assert(now() - t0 < 0.5 && fdclose_n == 2);
    usleep(300000);
    assert(fcntl(c1, F_GETFD) < 0 && fcntl(c2, F_GETFD) < 0);
    assert(fdclose_in_close(fdclose_pids[0]) && fdclose_in_close(fdclose_pids[1]));
    printf("test_fdclose per-fd: two stuck fds, two closers, both out of the table\n");
    t0 = now();
    assert(fdclose_quiesce() == 0);
    assert(fdclose_n == 0 && now() - t0 < 1.0);
    close(lsn);
    close(l2);
    printf("test_fdclose quiesce: closers in close() killed and reaped\n");

    pid_t idle = fork();
    if (idle == 0) { pause(); _exit(0); }
    fdclose_pids[0] = idle;
    fdclose_n = 1;
    t0 = now();
    assert(fdclose_quiesce() == -1 && fdclose_n == 1 && now() - t0 > 0.9);
    assert(kill(idle, 0) == 0);
    kill(idle, SIGKILL);
    waitpid(idle, NULL, 0);
    fdclose_n = 0;
    printf("test_fdclose quiesce: refuses while a closer is not in close(), leaves it alone\n");

    for (int i = 0; i < FDCLOSE_MAX_INFLIGHT; i++) fdclose_pids[i] = 1000000 + i;
    fdclose_n = FDCLOSE_MAX_INFLIGHT;
    fdclose_sock = -1;
    assert(fdclose_full() == 0);
    fdclose_nparked = FDCLOSE_PARK_PAUSE - FDCLOSE_MSG_FDS + 1;
    assert(fdclose_full() == 1);
    fdclose_n--;
    assert(fdclose_full() == 0);
    fdclose_n = 0;
    fdclose_nparked = 0;
    printf("test_fdclose backpressure: at the cap reads go on until a full message might not park\n");

    int many[FDCLOSE_MSG_FDS];
    for (int i = 0; i < FDCLOSE_MAX_INFLIGHT; i++) fdclose_pids[i] = 1000000 + i;
    fdclose_n = FDCLOSE_MAX_INFLIGHT;
    int slow = lingering(&lsn, 2);
    many[0] = slow;
    for (int i = 1; i < FDCLOSE_MSG_FDS; i++) many[i] = open("/dev/null", O_RDONLY);
    t0 = now();
    fd_close_detached(many, FDCLOSE_MSG_FDS);
    assert(now() - t0 < 0.5);
    assert(fdclose_n == FDCLOSE_MAX_INFLIGHT + 1 && fdclose_ovf && fdclose_nparked == 0);
    pid_t first = fdclose_ovf;
    int later[3];
    for (int i = 0; i < 3; i++) later[i] = open("/dev/null", O_RDONLY);
    fd_close_detached(later, 3);
    assert(fdclose_n == FDCLOSE_MAX_INFLIGHT + 1 && fdclose_ovf == first && fdclose_nparked == 3);
    assert(fcntl(later[0], F_GETFD) == 0);
    printf("test_fdclose overflow: a 253-fd message at the cap gets one closer; the next parks\n");
    assert(waitpid(first, NULL, 0) == first && now() - t0 > 1.5);
    for (int i = 0; i < FDCLOSE_MSG_FDS; i++) assert(fcntl(many[i], F_GETFD) < 0);
    assert(fdclose_reaped(first) == 1);
    assert(fdclose_ovf && fdclose_ovf != first && fdclose_nparked == 0 && fdclose_n == FDCLOSE_MAX_INFLIGHT + 1);
    pid_t second = fdclose_ovf;
    assert(waitpid(second, NULL, 0) == second && fdclose_reaped(second) == 1);
    for (int i = 0; i < 3; i++) assert(fcntl(later[i], F_GETFD) < 0);
    assert(!fdclose_ovf && fdclose_n == FDCLOSE_MAX_INFLIGHT);
    close(lsn);
    fdclose_n = 0;
    printf("test_fdclose overflow: reaping it hands the parked fds to the next; never past %d closers\n",
           FDCLOSE_MAX_INFLIGHT + 1);

    fdclose_sender_pid = 4242;
    fdclose_sender_uid = 65534;
    int z = open("/dev/null", O_RDONLY);
    fd_close_detached(&z, 1);
    assert(fdclose_n == 1 && fdclose_from_pid[0] == 4242 && fdclose_from_uid[0] == 65534);
    pid_t zp = waitpid(-1, NULL, 0);
    assert(fdclose_reaped(zp) == 1 && fdclose_n == 0);
    fdclose_sender_pid = 0;
    fdclose_sender_uid = (uid_t)-1;
    printf("test_fdclose sender: a closer remembers who sent its fd\n");

    printf("test_fdclose: OK\n");
    return 0;
}
