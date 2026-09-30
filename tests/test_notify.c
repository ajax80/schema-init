#include "../notify.h"
#include <assert.h>
#include <stdlib.h>

static void send_dgram(const char *path, const char *msg, int pass_fd) {
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(s >= 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    struct iovec iov = { (void *)msg, strlen(msg) };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_name = &a;
    mh.msg_namelen = sizeof a;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr al; } ctl;
    if (pass_fd >= 0) {
        mh.msg_control = ctl.b;
        mh.msg_controllen = sizeof ctl.b;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
    }
    assert(sendmsg(s, &mh, 0) == (ssize_t)strlen(msg));
    close(s);
}

int main(void) {
    struct notify_msg m;

    /* ---- parse ---- */
    const char *a = "READY=1\nSTATUS=Processing requests...\nMAINPID=42";
    assert(notify_parse(a, strlen(a), &m) == 1);
    assert(m.ready && !m.stopping && strcmp(m.status, "Processing requests...") == 0);
    const char *b = "READY=0\nFOO=bar";
    assert(notify_parse(b, strlen(b), &m) == 0 && !m.ready);
    const char *c = "READY=10\nREADY=1x";                 /* exact match only */
    assert(notify_parse(c, strlen(c), &m) == 0);
    const char *d = "STOPPING=1\nRELOADING=1\nWATCHDOG=1\n";
    assert(notify_parse(d, strlen(d), &m) == 1 && m.stopping && m.reloading && m.watchdog);
    char longs[400] = "STATUS=";
    memset(longs + 7, 'x', 300);
    longs[307] = '\0';
    assert(notify_parse(longs, strlen(longs), &m) == 1 && strlen(m.status) == NOTIFY_STATUS_MAX - 1);
    const char *e = "STATUS=a\tb\x1b[2Jc";                /* control chars neutralized */
    assert(notify_parse(e, strlen(e), &m) == 1 && !strchr(m.status, '\x1b') && !strchr(m.status, '\t'));
    printf("test_notify parse: OK\n");

    /* ---- cgroup attribution ---- */
    const char *cg = "/sys/fs/cgroup/schema-init/sshd";
    assert(notify_cgroup_text_matches("0::/schema-init/sshd\n", cg));
    assert(notify_cgroup_text_matches("0::/schema-init/sshd/worker\n", cg));
    assert(!notify_cgroup_text_matches("0::/schema-init/sshd-keygen\n", cg));   /* prefix trap */
    assert(!notify_cgroup_text_matches("0::/schema-init\n", cg));
    assert(!notify_cgroup_text_matches("0::/user.slice/x\n", cg));
    assert(!notify_cgroup_text_matches("1:name=x:/schema-init/sshd\n", cg));
    assert(notify_cgroup_text_matches("1:name=x:/y\n0::/schema-init/sshd\n", cg));
    assert(!notify_cgroup_text_matches("0::/schema-init/sshd\n", "/elsewhere/sshd"));
    assert(!notify_cgroup_text_matches("0::/\n", "/sys/fs/cgroup"));
    printf("test_notify cgroup match: OK\n");

    /* ---- socket round trip: kernel-attested sender pid ---- */
    char dir[] = "/tmp/schema-notify-XXXXXX";
    assert(mkdtemp(dir));
    char path[128];
    snprintf(path, sizeof path, "%s/notify", dir);
    int fd = notify_open(path);
    assert(fd >= 0);
    char buf[NOTIFY_MSG_MAX];
    pid_t pid = 0;
    assert(notify_recv(fd, buf, sizeof buf, &pid) == 0);          /* empty: drained */
    send_dgram(path, "READY=1", -1);
    ssize_t n = notify_recv(fd, buf, sizeof buf, &pid);
    assert(n == 7 && pid == getpid() && strcmp(buf, "READY=1") == 0);
    printf("test_notify recv creds: OK\n");

    /* ---- a message carrying fds is discarded and the fd is not leaked ---- */
    int probe = open("/dev/null", O_RDONLY | O_CLOEXEC);
    send_dgram(path, "READY=1\nFDSTORE=1", probe);
    close(probe);
    assert(notify_recv(fd, buf, sizeof buf, &pid) == -1);
    assert(notify_recv(fd, buf, sizeof buf, &pid) == 0);
    int nxt = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(nxt == probe);                                          /* lowest fd free again */
    close(nxt);
    printf("test_notify reject fds: OK\n");

    close(fd);
    unlink(path);
    rmdir(dir);
    printf("test_notify: ALL OK\n");
    return 0;
}
