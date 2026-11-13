#include "../ns.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>

static int try_create(const char *p) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return errno;
    close(fd);
    unlink(p);
    return 0;
}

int main(void) {
    if (geteuid() != 0) { printf("SKIP  test_mountns_rw (not root)\n"); return 0; }
    char rw[] = "/etc/schema-test-rw-XXXXXX", ro[] = "/etc/schema-test-ro-XXXXXX";
    char missing[64], f[96];
    assert(mkdtemp(rw) && mkdtemp(ro));
    snprintf(missing, sizeof missing, "%s/none", rw);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        const char *paths[] = { missing, rw };
        const char *step = "";
        if (apply_mount_ns("t", 0, PROTECT_SYSTEM_FULL, 0, paths, 2, &step) != 0) {
            fprintf(stderr, "apply_mount_ns: %s: %s\n", step, strerror(errno));
            _exit(10);
        }
        snprintf(f, sizeof f, "%s/f", rw);
        if (try_create(f) != 0) _exit(11);
        snprintf(f, sizeof f, "%s/f", ro);
        if (try_create(f) != EROFS) _exit(12);
        if (try_create("/etc/schema-test-rw-sibling") != EROFS) _exit(13);
        if (try_create("/usr/schema-test-rw-sibling") != EROFS) _exit(14);
        _exit(0);
    }
    int st;
    assert(waitpid(pid, &st, 0) == pid);
    rmdir(rw);
    rmdir(ro);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "test_mountns_rw: child status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 1;
    }

    /* an rw path above protect_home's /run/user must not make that tmpfs writable */
    pid = fork();
    if (pid == 0) {
        const char *run[] = { "/run" };
        const char *step = "";
        if (apply_mount_ns("t", 0, 0, 1, run, 1, &step) != 0) _exit(30);
        if (try_create("/run/user/schema-test-rw") != EROFS) _exit(31);
        _exit(0);
    }
    assert(waitpid(pid, &st, 0) == pid);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "test_mountns_rw: protect_home child status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 1;
    }

    /* no read-only knob: rw paths alone create no namespace */
    const char *paths[] = { "/etc" };
    const char *step = "";
    pid = fork();
    if (pid == 0) {
        char before[64], after[64];
        snprintf(before, sizeof before, "%ld", (long)0);
        ssize_t n1 = readlink("/proc/self/ns/mnt", before, sizeof before - 1);
        if (apply_mount_ns("t", 0, 0, 0, paths, 1, &step) != 0) _exit(20);
        ssize_t n2 = readlink("/proc/self/ns/mnt", after, sizeof after - 1);
        if (n1 < 0 || n1 != n2 || memcmp(before, after, (size_t)n1) != 0) _exit(21);
        _exit(0);
    }
    assert(waitpid(pid, &st, 0) == pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    /* audit 2026-11-12, 1.7: private_tmp is a fresh dir on the host /tmp's
     * filesystem (its limit), not a tmpfs of its own (half the RAM) */
    const char *pt = "/tmp/schema-private/schema-test-pt";
    struct statfs host;
    assert(statfs("/tmp", &host) == 0);
    for (int round = 0; round < 2; round++) {
        pid = fork();
        if (pid == 0) {
            struct statfs in;
            struct stat sb;
            if (apply_mount_ns("schema-test-pt", 1, 0, 0, NULL, 0, &step) != 0) _exit(40);
            if (statfs("/tmp", &in) != 0 || in.f_type != host.f_type) _exit(41);
            /* fresh each spawn: the last round's file is gone */
            if (access("/tmp/left", F_OK) == 0) _exit(42);
            if (try_create("/tmp/x") != 0 || try_create("/var/tmp/x") != 0) _exit(43);
            if (stat("/tmp", &sb) != 0 || (sb.st_mode & 07777) != 01777) _exit(44);
            int fd = open("/tmp/left", O_WRONLY | O_CREAT, 0644);
            if (fd < 0) _exit(45);
            close(fd);
            fd = open("/var/tmp/left-var", O_WRONLY | O_CREAT, 0644);
            if (fd < 0) _exit(46);
            close(fd);
            _exit(0);
        }
        assert(waitpid(pid, &st, 0) == pid);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            fprintf(stderr, "test_mountns_rw: private_tmp round %d status %d\n", round,
                    WIFEXITED(st) ? WEXITSTATUS(st) : -1);
            return 1;
        }
        snprintf(f, sizeof f, "%s/tmp/left", pt);
        assert(access(f, F_OK) == 0);
        snprintf(f, sizeof f, "%s/var-tmp/left-var", pt);
        assert(access(f, F_OK) == 0);
    }
    struct stat sp;
    assert(stat("/tmp/schema-private", &sp) == 0 && sp.st_uid == 0 && (sp.st_mode & 0777) == 0700);
    snprintf(f, sizeof f, "%s/tmp/left", pt); unlink(f);
    snprintf(f, sizeof f, "%s/var-tmp/left-var", pt); unlink(f);
    snprintf(f, sizeof f, "%s/tmp", pt); rmdir(f);
    snprintf(f, sizeof f, "%s/var-tmp", pt); rmdir(f);
    rmdir(pt);
    printf("test_mountns_rw: ok\n");
    return 0;
}
