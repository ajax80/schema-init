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

    /* healthy to healthy does not restart the window: a timer or socket-activated
     * service cycles PERFECT <-> FULL_TRUST all day, and the box must still settle */
    memset(&st, 0, sizeof st);
    assert(settle_step(&st, kv("PERFECT", 0), S(0), 60) == 0);
    assert(settle_step(&st, kv("FULL_TRUST", 0), S(20), 60) == 0);
    assert(settle_step(&st, kv("PERFECT", 0), S(40), 60) == 0);
    assert(settle_step(&st, kv("FUNDAMENTAL", 0), S(61), 60) == 1);

    /* a failure parked in EXCISED or DORMANT never settles */
    memset(&st, 0, sizeof st);
    assert(settle_step(&st, kv("DORMANT", 0), S(0), 60) == 0);
    assert(settle_step(&st, kv("DORMANT", 0), S(200), 60) == 0);
    memset(&st, 0, sizeof st);
    char x[1024], *r;
    snprintf(x, sizeof x, "%s", kv("EXCISED", 0));
    r = strstr(x, "service.b.excise_reason=none\n") + 24;
    memmove(r + 9, r + 4, strlen(r + 4) + 1);
    memcpy(r, "exit-code", 9);
    assert(strstr(x, "service.b.excise_reason=exit-code\nservice.c."));
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

/* an init=/sbin/init symlink install has comm "init"; the exe link names the binary */
static void test_pid1_exe(void) {
    assert(pid1_exe_is_schema_init("/usr/bin/schema-init"));
    assert(pid1_exe_is_schema_init("/usr/bin/schema-init (deleted)"));
    assert(pid1_exe_is_schema_init("/var/lib/schema-init/known-good/schema-init"));
    assert(!pid1_exe_is_schema_init("/usr/lib/systemd/systemd"));
    assert(!pid1_exe_is_schema_init("/usr/lib/schema-init/init"));
    assert(!pid1_exe_is_schema_init(""));
}

/* after a live re-exec the running image never booted: its baseline would vouch for nothing */
static void test_reexecd_since_boot(void) {
    const char *kv = "services_count=2\ngroups_count=0\nbooted_version=0.4.27-1.fc44\nservice.a.pid=1\n";
    assert(reexecd_since_boot(kv, "schema-init 0.4.28-1.fc44\n", "64769:1234") == 1);
    assert(reexecd_since_boot(kv, "schema-init 0.4.27-1.fc44\n", "64769:1234") == 0);
    assert(reexecd_since_boot(kv, "schema-init 0.4.27-1.fc44", "64769:1234") == 0);
    /* a prefix is not the same version */
    assert(reexecd_since_boot(kv, "schema-init 0.4.27-1.fc4\n", "64769:1234") == 1);
    /* an older PID 1 without the line: proceed */
    assert(reexecd_since_boot("services_count=0\ngroups_count=0\n", "schema-init 0.4.28\n", "64769:1234") == 0);
    /* a service named booted_version is not the global line */
    assert(reexecd_since_boot("service.booted_version=x\n", "schema-init 0.4.28\n", "64769:1234") == 0);
    /* a --version that isn't schema-init's can't be compared: refuse */
    assert(reexecd_since_boot(kv, "", "64769:1234") == 1);

    /* booted_image=: the image PID 1 booted from (dev:ino); a same-version rebuild re-exec'd
     * in is another inode */
    const char *ki = "services_count=2\ngroups_count=0\nbooted_version=0.4.27-1.fc44\nbooted_image=64769:1234\n";
    assert(reexecd_since_boot(ki, "schema-init 0.4.27-1.fc44\n", "64769:1234") == 0);
    assert(reexecd_since_boot(ki, "schema-init 0.4.27-1.fc44\n", "64769:9999") == 1);
    assert(reexecd_since_boot(ki, "schema-init 0.4.27-1.fc44\n", "64769:12345") == 1);
    /* the version still counts with a matching image */
    assert(reexecd_since_boot(ki, "schema-init 0.4.28-1.fc44\n", "64769:1234") == 1);
    /* not a line of its own: not the field */
    assert(reexecd_since_boot("service.booted_image=1:2\nbooted_version=0.4.27-1.fc44\n", "schema-init 0.4.27-1.fc44\n",
                              "64769:1234") == 0);
}

/* a window near or past the fixed 15 min would give up before it could settle */
static void test_give_up(void) {
    assert(settle_give_up_ms(60) == S(15 * 60));
    assert(settle_give_up_ms(900) == S(900 + 5 * 60));
    assert(settle_give_up_ms(3600) == S(3600 + 5 * 60));
}

/* svc_failed()'s rule through failed=, and a socket-held NEW_PROCESS is idle, not starting */
static void test_settle_engine_rule(void) {
    settle_t st = {0};
    char x[1024], y[1200];
    /* EXCISED exit-code but failed=0: a stop's hold that a start timeout overtook */
    snprintf(x, sizeof x, "service.a.state=EXCISED\nservice.a.restarts=0\nservice.a.excise_reason=exit-code\n"
             "service.a.failed=0\n");
    assert(settle_step(&st, x, S(0), 60) == 0);
    assert(settle_step(&st, x, S(61), 60) == 1);
    memset(&st, 0, sizeof st);
    snprintf(x, sizeof x, "service.a.state=DORMANT\nservice.a.restarts=0\nservice.a.excise_reason=none\n"
             "service.a.failed=0\n");
    assert(settle_step(&st, x, S(0), 60) == 0);
    assert(settle_step(&st, x, S(61), 60) == 1);
    /* failed=1 wins even with a reason the old rule called fine */
    memset(&st, 0, sizeof st);
    snprintf(x, sizeof x, "service.a.state=EXCISED\nservice.a.restarts=0\nservice.a.excise_reason=none\n"
             "service.a.failed=1\n");
    assert(settle_step(&st, x, S(0), 60) == 0);
    assert(settle_step(&st, x, S(61), 60) == 0);
    /* failed but critical=0 (mcelog on an unsupported CPU): settles; critical=1 doesn't */
    memset(&st, 0, sizeof st);
    snprintf(x, sizeof x, "service.mcelog.state=EXCISED\nservice.mcelog.restarts=0\nservice.mcelog.excise_reason=exit-code\n"
             "service.mcelog.failed=1\nservice.mcelog.critical=0\nservice.b.state=PERFECT\nservice.b.restarts=0\n"
             "service.b.failed=0\nservice.b.critical=0\n");
    assert(settle_step(&st, x, S(0), 60) == 0);
    assert(settle_step(&st, x, S(61), 60) == 1);
    {
        char fl[64];
        assert(settle_failed(x, fl, sizeof fl) == 6 && !strcmp(fl, "mcelog"));
    }
    {
        /* several names space-joined; one that won't fit is left out whole */
        char fl[12], y[512];
        snprintf(y, sizeof y, "service.aa.failed=1\nservice.aa.critical=0\nservice.bbbbbbbbbb.failed=1\n"
                 "service.bbbbbbbbbb.critical=0\nservice.cc.failed=1\nservice.cc.critical=0\n");
        assert(settle_failed(y, fl, sizeof fl) == 5 && !strcmp(fl, "aa cc"));
    }
    memset(&st, 0, sizeof st);
    snprintf(x, sizeof x, "service.a.state=DORMANT\nservice.a.restarts=3\nservice.a.excise_reason=exit-code\n"
             "service.a.failed=1\nservice.a.critical=1\n");
    assert(settle_step(&st, x, S(0), 60) == 0);
    assert(settle_step(&st, x, S(61), 60) == 0);
    {
        char fl[64];
        assert(settle_failed(x, fl, sizeof fl) == 0 && !fl[0]);
    }
    /* waiting on its socket */
    memset(&st, 0, sizeof st);
    snprintf(x, sizeof x, "service.a.state=NEW_PROCESS\nservice.a.restarts=0\nservice.a.excise_reason=none\n");
    snprintf(y, sizeof y, "%sservice.a.waiting=/run/a.sock\n", x);
    assert(settle_step(&st, y, S(0), 60) == 0);
    assert(settle_step(&st, y, S(61), 60) == 1);
    memset(&st, 0, sizeof st);
    assert(settle_step(&st, x, S(0), 60) == 0);
    assert(settle_step(&st, x, S(61), 60) == 0);
    /* a name that is a prefix of another must not borrow its fields */
    memset(&st, 0, sizeof st);
    snprintf(y, sizeof y, "%sservice.ab.state=PERFECT\nservice.ab.restarts=0\nservice.ab.waiting=/run/b\n", x);
    assert(settle_step(&st, y, S(0), 60) == 0);
    assert(settle_step(&st, y, S(61), 60) == 0);
}

int main(int argc, char **argv) {
    snprintf(base, sizeof base, "%s", argc > 1 ? argv[1] : ".");
    test_atomic_perms();
    test_symlink_refused();
    test_bad_input_refused();
    test_dir_mode_and_owner();
    test_tmpfs_refused();
    test_settle();
    test_settle_engine_rule();
    test_pid1_exe();
    test_give_up();
    test_reexecd_since_boot();
    puts("test_verify_baseline: ok");
    return 0;
}
