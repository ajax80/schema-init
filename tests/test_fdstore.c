#include "../fdstore.h"
#include <assert.h>
#include <stdio.h>

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

    printf("test_fdstore: OK\n");
    return 0;
}
