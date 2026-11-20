#include "../verify_baseline.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

static const char out[] =
    "schema-verify-baseline v1\n"
    "binary_version=0.4.99\n"
    "cmdline.root=UUID=abc\n"
    "svc.a.exec=/bin/a\n";

static char base[4096];

static char *mkdir_t(const char *tag) {
    static char p[4200];
    snprintf(p, sizeof p, "%s/vb-%s-XXXXXX", base, tag);
    assert(mkdtemp(p));
    return p;
}

static char *slurp(const char *path) {
    static char buf[4096];
    int fd = open(path, O_RDONLY);
    ssize_t n;
    assert(fd >= 0);
    n = read(fd, buf, sizeof buf - 1);
    assert(n >= 0);
    buf[n] = '\0';
    close(fd);
    return buf;
}

static void test_atomic_perms(void) {
    char *d = mkdir_t("perms"), p[4300];
    struct stat st;
    assert(baseline_write(d, out, sizeof out - 1, 1763500000) == 0);
    snprintf(p, sizeof p, "%s/baseline", d);
    assert(lstat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0600);
    assert(!strcmp(slurp(p),
        "schema-verify-baseline v1\n"
        "binary_version=0.4.99\n"
        "created=1763500000\n"
        "cmdline.root=UUID=abc\n"
        "svc.a.exec=/bin/a\n"));
    snprintf(p, sizeof p, "%s/established", d);
    assert(access(p, F_OK) == 0);
    snprintf(p, sizeof p, "%s/baseline.tmp", d);
    assert(access(p, F_OK) < 0);
    /* a rewrite replaces it */
    assert(baseline_write(d, out, sizeof out - 1, 1763500001) == 0);
    snprintf(p, sizeof p, "%s/baseline", d);
    assert(strstr(slurp(p), "\ncreated=1763500001\n"));
}

static void test_symlink_refused(void) {
    char *d = mkdir_t("link"), tgt[4300], p[4300], dl[4300];
    int fd;
    snprintf(tgt, sizeof tgt, "%s/target", d);
    fd = open(tgt, O_WRONLY | O_CREAT, 0644);
    assert(fd >= 0 && write(fd, "keep\n", 5) == 5);
    close(fd);
    snprintf(p, sizeof p, "%s/baseline", d);
    assert(symlink(tgt, p) == 0);
    errno = 0;
    assert(baseline_write(d, out, sizeof out - 1, 1) == -1 && errno == ELOOP);
    assert(!strcmp(slurp(tgt), "keep\n"));

    /* the dir itself a symlink */
    snprintf(dl, sizeof dl, "%s-dirlink", d);
    assert(symlink(d, dl) == 0);
    errno = 0;
    assert(baseline_write(dl, out, sizeof out - 1, 1) == -1 && errno == ELOOP);
    unlink(dl);
}

static void test_bad_input_refused(void) {
    char *d = mkdir_t("bad"), p[4300];
    static const char old[] = "schema-init: unknown option '--check'\n";
    errno = 0;
    assert(baseline_write(d, old, sizeof old - 1, 1) == -1 && errno == EINVAL);
    snprintf(p, sizeof p, "%s/established", d);
    assert(access(p, F_OK) < 0);
}

static void test_dir_mode_and_owner(void) {
    char *d = mkdir_t("mode");
    struct stat st;
    assert(chmod(d, 0755) == 0);
    assert(baseline_write(d, out, sizeof out - 1, 1) == 0);
    assert(stat(d, &st) == 0 && (st.st_mode & 07777) == 0700);

    /* as root every dir is "ours", and "/" would really be written to */
    if (geteuid() == 0) {
        puts("test_dir_mode_and_owner: foreign-owner case skipped (running as root)");
        return;
    }
    if (stat("/usr", &st) < 0 || st.st_uid == geteuid()) {
        puts("test_dir_mode_and_owner: foreign-owner case skipped (no foreign-owned /usr)");
        return;
    }
    errno = 0;
    assert(baseline_write("/usr", out, sizeof out - 1, 1) == -1 && errno == EPERM);
}

static void test_tmpfs_refused(void) {
    struct statfs sf;
    char d[] = "/dev/shm/vb-tmpfs-XXXXXX";
    if (statfs("/dev/shm", &sf) < 0 || sf.f_type != TMPFS_MAGIC || !mkdtemp(d)) {
        puts("test_tmpfs_refused: skipped (no tmpfs /dev/shm)");
        return;
    }
    errno = 0;
    assert(baseline_write(d, out, sizeof out - 1, 1) == -1 && errno == EROFS);
    rmdir(d);
}

static const char *kv(const char *xstate, int xrestarts) {
    static char b[1024];
    snprintf(b, sizeof b,
        "services_count=3\ngroups_count=0\n"
        "service.a.pid=10\nservice.a.state=FUNDAMENTAL\nservice.a.restarts=0\nservice.a.excise_reason=none\n"
        "service.b.pid=11\nservice.b.state=%s\nservice.b.restarts=%d\nservice.b.excise_reason=none\n"
        "service.c.pid=0\nservice.c.state=PERFECT\nservice.c.restarts=0\nservice.c.excise_reason=none\n",
        xstate, xrestarts);
    return b;
}

#define S(n) ((uint64_t)(n) * 1000)

static void test_settle(void) {
    settle_t st = {0};
    assert(settle_step(&st, kv("FUNDAMENTAL", 0), S(0), 60) == 0);
    assert(settle_step(&st, kv("FUNDAMENTAL", 0), S(30), 60) == 0);
    assert(settle_step(&st, kv("FUNDAMENTAL", 0), S(61), 60) == 1);

    memset(&st, 0, sizeof st);
    assert(settle_step(&st, kv("SETTLED", 0), S(0), 60) == 0);
    assert(settle_step(&st, kv("SETTLED", 1), S(50), 60) == 0);
    assert(settle_step(&st, kv("SETTLED", 1), S(61), 60) == 0);
    assert(settle_step(&st, kv("SETTLED", 1), S(100), 60) == 0);
    assert(settle_step(&st, kv("SETTLED", 1), S(111), 60) == 1);

    memset(&st, 0, sizeof st);
    assert(settle_step(&st, kv("FULL_TRUST", 0), S(0), 60) == 0);
    assert(settle_step(&st, kv("RECOVERY", 0), S(40), 60) == 0);
    assert(settle_step(&st, kv("FULL_TRUST", 0), S(61), 60) == 0);
    assert(settle_step(&st, kv("FULL_TRUST", 0), S(99), 60) == 0);
    assert(settle_step(&st, kv("FULL_TRUST", 0), S(101), 60) == 1);

    /* a failure parked in EXCISED or DORMANT never settles */
    memset(&st, 0, sizeof st);
    assert(settle_step(&st, kv("DORMANT", 0), S(0), 60) == 0);
    assert(settle_step(&st, kv("DORMANT", 0), S(200), 60) == 0);
    memset(&st, 0, sizeof st);
    char x[1024];
    snprintf(x, sizeof x, "%s", kv("EXCISED", 0));
    memcpy(strstr(x, "service.b.excise_reason=none"), "service.b.excise_reason=exit", 28);
    assert(settle_step(&st, x, S(0), 60) == 0);
    assert(settle_step(&st, x, S(200), 60) == 0);
    /* a deliberate stop (EXCISED, reason none) is not a failure */
    memset(&st, 0, sizeof st);
    assert(settle_step(&st, kv("EXCISED", 0), S(0), 60) == 0);
    assert(settle_step(&st, kv("EXCISED", 0), S(61), 60) == 1);
    /* a service appearing changes the set */
    memset(&st, 0, sizeof st);
    assert(settle_step(&st, "service.a.state=PERFECT\nservice.a.restarts=0\n", S(0), 60) == 0);
    assert(settle_step(&st, kv("PERFECT", 0), S(61), 60) == 0);
    assert(settle_step(&st, kv("PERFECT", 0), S(121), 60) == 1);
}

int main(int argc, char **argv) {
    snprintf(base, sizeof base, "%s", argc > 1 ? argv[1] : ".");
    test_atomic_perms();
    test_symlink_refused();
    test_bad_input_refused();
    test_dir_mode_and_owner();
    test_tmpfs_refused();
    test_settle();
    puts("test_verify_baseline: ok");
    return 0;
}
