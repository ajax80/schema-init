#include "../notify.h"
#include <assert.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/wait.h>

static int count_fds(void) {
    int c = 0;
    DIR *d = opendir("/proc/self/fd");
    assert(d);
    while (readdir(d)) c++;
    closedir(d);
    return c - 1;
}

static void send_fds(const char *path, const char *msg, const int *fds, int nfds) {
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
    static char ctl[CMSG_SPACE(sizeof(int) * NOTIFY_MAX_FDS)];
    memset(ctl, 0, sizeof ctl);
    mh.msg_control = ctl;
    mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    assert(sendmsg(s, &mh, 0) == (ssize_t)strlen(msg));
    close(s);
}

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
    const char *bn = "BUSNAME=org.freedesktop.NetworkManager\nBUSOWNERPID=1234";
    assert(notify_parse(bn, strlen(bn), &m) == 1 && !m.ready);
    assert(strcmp(m.busname, "org.freedesktop.NetworkManager") == 0 && m.bus_owner_pid == 1234);
    const char *bad = "BUSOWNERPID=123456789012345678901234";       /* overlong: ignored */
    assert(notify_parse(bad, strlen(bad), &m) == 0 && m.bus_owner_pid == 0);
    assert(!notify_sender_is_broker(getpid(), getuid()));             /* not root schema-dbus */
    assert(notify_broker_exe_ok("/usr/bin/schema-dbus"));
    assert(notify_broker_exe_ok("/usr/local/bin/schema-dbus"));
    assert(notify_broker_exe_ok("/usr/local/bin/schema-dbus (deleted)"));  /* upgraded under it */
    assert(!notify_broker_exe_ok("/tmp/schema-dbus"));
    assert(!notify_broker_exe_ok("/usr/bin/schema-dbus-evil"));
    assert(!notify_broker_exe_ok("/usr/bin/schema-dbus (deleted) "));
    assert(!notify_broker_exe_ok(" (deleted)"));
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

    /* ---- reading a live sender's cgroup; an exited sender can't be attributed ---- */
    char cgt[NOTIFY_CGROUP_TEXT_MAX];
    assert(notify_read_cgroup(getpid(), cgt, sizeof cgt) == 1 && strstr(cgt, "::"));
    pid_t gone = fork();
    if (gone == 0) _exit(0);
    waitpid(gone, NULL, 0);
    assert(notify_read_cgroup(gone, cgt, sizeof cgt) == 0);
    printf("test_notify read cgroup: OK\n");

    /* ---- socket round trip: kernel-attested sender pid ---- */
    char dir[] = "/tmp/schema-notify-XXXXXX";
    assert(mkdtemp(dir));
    char path[128];
    snprintf(path, sizeof path, "%s/notify", dir);
    int fd = notify_open(path);
    assert(fd >= 0);
    char buf[NOTIFY_MSG_MAX];
    int rf[NOTIFY_MAX_FDS], nrf = 0;
    pid_t pid = 0;
    uid_t uid = 0;
    assert(notify_recv(fd, buf, sizeof buf, &pid, &uid, rf, &nrf) == 0);          /* empty: drained */
    send_dgram(path, "READY=1", -1);
    ssize_t n = notify_recv(fd, buf, sizeof buf, &pid, &uid, rf, &nrf);
    assert(n == 7 && pid == getpid() && uid == getuid() && strcmp(buf, "READY=1") == 0);
    printf("test_notify recv creds: OK\n");

    /* ---- FDSTORE keys ---- */
    const char *f1 = "FDSTORE=1\nFDNAME=conn";
    assert(notify_parse(f1, strlen(f1), &m) == 1);
    assert(m.fdstore == 1 && !m.fdstore_remove && strcmp(m.fdname, "conn") == 0);
    const char *f2 = "FDSTOREREMOVE=1\nFDNAME=state";
    assert(notify_parse(f2, strlen(f2), &m) == 1);
    assert(m.fdstore_remove == 1 && !m.fdstore && strcmp(m.fdname, "state") == 0);
    char lg[200];
    snprintf(lg, sizeof lg, "FDSTORE=1\nFDNAME=%0*d", 64, 0);
    assert(notify_parse(lg, strlen(lg), &m) == 1 && m.fdstore == 1);
    assert(strcmp(m.fdname, ":") == 0);
    snprintf(lg, sizeof lg, "FDSTORE=1\nFDNAME=%0*d", 63, 0);
    assert(notify_parse(lg, strlen(lg), &m) == 1 && strlen(m.fdname) == 63);
    printf("test_notify parse fdstore: OK\n");

    /* ---- fds are handed to the caller ---- */
    int base = count_fds();
    int pp[3][2], sendv[3];
    for (int i = 0; i < 3; i++) { assert(pipe(pp[i]) == 0); sendv[i] = pp[i][0]; }
    send_fds(path, "FDSTORE=1", sendv, 3);
    n = notify_recv(fd, buf, sizeof buf, &pid, &uid, rf, &nrf);
    assert(n == 9 && nrf == 3);
    for (int i = 0; i < 3; i++) {
        assert(fcntl(rf[i], F_GETFD) & FD_CLOEXEC);
        close(rf[i]);
        close(pp[i][0]);
        close(pp[i][1]);
    }
    assert(notify_recv(fd, buf, sizeof buf, &pid, &uid, rf, &nrf) == 0 && nrf == 0);
    assert(count_fds() == base);
    printf("test_notify recv fds: OK\n");

    int many[NOTIFY_MAX_FDS];
    for (int i = 0; i < NOTIFY_MAX_FDS; i++) { many[i] = open("/dev/null", O_RDONLY | O_CLOEXEC); assert(many[i] >= 0); }
    send_fds(path, "FDSTORE=1", many, NOTIFY_MAX_FDS);
    n = notify_recv(fd, buf, sizeof buf, &pid, &uid, rf, &nrf);
    assert(n == 9 && nrf == NOTIFY_MAX_FDS);
    for (int i = 0; i < NOTIFY_MAX_FDS; i++) { close(rf[i]); close(many[i]); }
    assert(count_fds() == base);
    printf("test_notify recv 253 fds: OK\n");

    /* ---- truncated datagram: discarded, fds closed ---- */
    int tr[2];
    tr[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    tr[1] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    send_fds(path, "FDSTORE=1\nFDNAME=0123456789", tr, 2);
    close(tr[0]);
    close(tr[1]);
    char small[8];
    assert(notify_recv(fd, small, sizeof small, &pid, &uid, rf, &nrf) == -1 && nrf == 0);
    assert(notify_recv(fd, buf, sizeof buf, &pid, &uid, rf, &nrf) == 0);
    assert(count_fds() == base);
    printf("test_notify truncated closes fds: OK\n");

    close(fd);
    unlink(path);
    rmdir(dir);
    printf("test_notify: ALL OK\n");
    return 0;
}
