#include "../fdstore.h"
#include <assert.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>

fdstore_ent *fdstore_v;
int fdstore_n, fdstore_cap;

static int mkfd(void) {
    int p[2];
    assert(pipe(p) == 0);
    close(p[1]);
    return p[0];
}

static int is_open(int fd) { return fcntl(fd, F_GETFD) >= 0; }

static int keep_a(const char *svc, void *ctx) {
    (void)ctx;
    return strcmp(svc, "a") == 0;
}

int main(void) {
    assert(fdstore_v == NULL);

    int f1 = mkfd(), f2 = mkfd(), f3 = mkfd();
    assert(fdstore_add("dbus", f1, "listen", 8) == 0);
    assert(fdstore_add("dbus", f2, "conn", 8) == 0);
    assert(fdstore_add("dbus", f3, "conn", 8) == 0);
    int fds[8];
    const char *names[8];
    assert(fdstore_count("dbus") == 3);
    assert(fdstore_collect("dbus", fds, names, 8) == 3);
    assert(fds[0] == f1 && fds[1] == f2 && fds[2] == f3);
    assert(!strcmp(names[0], "listen") && !strcmp(names[1], "conn") && !strcmp(names[2], "conn"));

    int o = mkfd();
    assert(fdstore_add("other", o, NULL, 8) == 0);
    assert(fdstore_collect("other", fds, names, 8) == 1);
    assert(!strcmp(names[0], "stored"));

    fdstore_set_cloexec(1);
    assert(fcntl(f1, F_GETFD) & FD_CLOEXEC);
    fdstore_set_cloexec(0);
    assert(!(fcntl(f1, F_GETFD) & FD_CLOEXEC));

    assert(fdstore_remove("dbus", "conn") == 2);
    assert(!is_open(f2) && !is_open(f3) && is_open(f1));
    assert(fdstore_count("dbus") == 1);

    assert(fdstore_drop("dbus") == 1);
    assert(fdstore_count("dbus") == 0 && !is_open(f1));
    assert(is_open(o) && fdstore_count("other") == 1);
    assert(fdstore_drop("other") == 1 && !is_open(o));

    int c1 = mkfd(), c2 = mkfd(), c3 = mkfd();
    assert(fdstore_add("cap", c1, "x", 2) == 0);
    assert(fdstore_add("cap", c2, "y", 2) == 0);
    assert(fdstore_add("cap", c3, "z", 2) == -1);
    assert(!is_open(c3) && is_open(c1) && is_open(c2));
    assert(fdstore_add("cap", mkfd(), "w", 0) == -1);
    assert(fdstore_drop("cap") == 2);

    int b1 = mkfd(), b2 = mkfd();
    assert(fdstore_add("bad", b1, "a:b", 8) == -1 && !is_open(b1));
    char longname[65];
    memset(longname, 'n', 64);
    longname[64] = 0;
    assert(fdstore_add("bad", b2, longname, 8) == -1 && !is_open(b2));
    assert(fdstore_count("bad") == 0);

    int sa = mkfd(), sb = mkfd();
    assert(fdstore_add("a", sa, "s", 8) == 0);
    assert(fdstore_add("b", sb, "s", 8) == 0);
    assert(fdstore_sweep(keep_a, NULL) == 1);
    assert(is_open(sa) && !is_open(sb));
    assert(fdstore_count("a") == 1 && fdstore_count("b") == 0);
    assert(fdstore_drop("a") == 1);

    assert(fdstore_add("dbus", mkfd(), "listen", 8) == 0);
    assert(fdstore_add("dbus", mkfd(), "state", 8) == 0);
    assert(fdstore_add("dbus", mkfd(), "conn", 8) == 0);
    char nm[16384];
    assert(fdstore_fdnames("dbus", 1, nm, sizeof nm) > 0);
    assert(strcmp(nm, "dbus.socket:listen:state:conn") == 0);
    assert(fdstore_fdnames("dbus", 0, nm, sizeof nm) > 0 && strcmp(nm, "listen:state:conn") == 0);
    char tiny[32];
    assert(fdstore_fdnames("dbus", 1, tiny, sizeof tiny) > 0);
    assert(fdstore_drop("dbus") == 3);
    for (int i = 0; i < 300; i++) assert(fdstore_add("dbus", mkfd(), "conn", 400) == 0);
    assert(fdstore_fdnames("dbus", 1, nm, sizeof nm) > 0);
    assert(strncmp(nm, "dbus.socket:conn:conn", 21) == 0);
    assert(fdstore_fdnames("dbus", 1, tiny, sizeof tiny) == -1);
    assert(fdstore_drop("dbus") == 300);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        struct rlimit rl;
        assert(getrlimit(RLIMIT_NOFILE, &rl) == 0);
        rl.rlim_cur = 1024;
        if (rl.rlim_max < 1500) _exit(0);
        assert(setrlimit(RLIMIT_NOFILE, &rl) == 0);
        for (int i = 0; i < 40; i++) assert(open("/dev/null", O_RDONLY) >= 0);
        int src[700];
        for (int i = 0; i < 700; i++) {
            src[i] = open("/dev/null", O_RDONLY | O_CLOEXEC);
            assert(src[i] >= 0);
        }
        assert(fdstore_place_fds(src, 700) == 0);
        for (int i = 0; i < 700; i++) assert(is_open(3 + i));
        assert(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == 1024);
        _exit(0);
    }
    int st;
    assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        struct rlimit rl;
        assert(getrlimit(RLIMIT_NOFILE, &rl) == 0);
        rl.rlim_cur = 1500;
        if (rl.rlim_max < 2000) _exit(0);
        assert(setrlimit(RLIMIT_NOFILE, &rl) == 0);
        for (int i = 0; i < 200; i++) assert(open("/dev/null", O_RDONLY) >= 0);
        int src[700];
        for (int i = 0; i < 700; i++) {
            src[i] = open("/dev/null", O_RDONLY | O_CLOEXEC);
            assert(src[i] >= 0);
        }
        assert(fdstore_place_fds(src, 700) == 0);
        for (int i = 0; i < 700; i++) assert(is_open(3 + i));
        assert(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == 1500);
        _exit(0);
    }
    assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);

    printf("test_fdstore: OK\n");
    return 0;
}
