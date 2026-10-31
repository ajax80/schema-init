#include "../ns.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
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
        if (apply_mount_ns(0, PROTECT_SYSTEM_FULL, 0, paths, 2, &step) != 0) {
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
        if (apply_mount_ns(0, 0, 1, run, 1, &step) != 0) _exit(30);
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
        if (apply_mount_ns(0, 0, 0, paths, 1, &step) != 0) _exit(20);
        ssize_t n2 = readlink("/proc/self/ns/mnt", after, sizeof after - 1);
        if (n1 < 0 || n1 != n2 || memcmp(before, after, (size_t)n1) != 0) _exit(21);
        _exit(0);
    }
    assert(waitpid(pid, &st, 0) == pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("test_mountns_rw: ok\n");
    return 0;
}
