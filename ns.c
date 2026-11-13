#include "ns.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>

int parse_ns_bool(const char *val, uint8_t *out) {
    if (strcmp(val, "0") == 0) { *out = 0; return 0; }
    if (strcmp(val, "1") == 0) { *out = 1; return 0; }
    return -1;
}

int parse_protect_system(const char *val, uint8_t *out) {
    if (strcmp(val, "full") == 0) { *out = PROTECT_SYSTEM_FULL; return 0; }
    return parse_ns_bool(val, out);
}

/* bind p onto itself, then set (ro=1) or clear (ro=0) MOUNT_ATTR_RDONLY */
static int bind_attr(const char *p, int ro) {
    struct { uint64_t attr_set, attr_clr, propagation, userns_fd; } attr = { 0 };
    if (ro) attr.attr_set = 0x1;
    else    attr.attr_clr = 0x1;
    if (mount(p, p, NULL, MS_BIND | MS_REC, NULL) != 0)
        return errno == ENOENT ? 0 : -1;
    return (int)syscall(SYS_mount_setattr, AT_FDCWD, p, AT_RECURSIVE,
                        &attr, sizeof(attr));
}

static int mount_tmpfs(const char *p, unsigned long flags, const char *opts) {
    if (mount("tmpfs", p, "tmpfs", flags, opts) != 0)
        return errno == ENOENT ? 0 : -1;
    return 0;
}

/* rm -rf of dfd's entry `name`, never following a symlink: the tree is the
 * service's own, written as whatever user it ran as. Best effort and depth
 * capped (an fd per level): nothing waits on it emptying */
static void rmtree_at(int dfd, const char *name, int depth) {
    int fd = depth < 64 ? openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (fd >= 0) {
        DIR *d = fdopendir(fd);
        struct dirent *e;
        if (!d) { close(fd); return; }
        while ((e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                rmtree_at(fd, e->d_name, depth + 1);
        closedir(d);
        unlinkat(dfd, name, AT_REMOVEDIR);
    } else if (depth < 64) {
        unlinkat(dfd, name, 0);
    }
}

/* a name nobody can plant ahead of time */
static void aside_name(char *buf, size_t n, const char *base) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(buf, n, "%s.old-%d-%ld%09ld", base, (int)getpid(), (long)ts.tv_sec, ts.tv_nsec);
}

/* fd of a fresh empty dir dfd/name, mode set exactly (umask aside). The old
 * one is renamed away first, so whatever its removal leaves behind can't stop
 * the new one being made */
static int fresh_dir(int dfd, const char *name, mode_t mode) {
    char old[300];
    aside_name(old, sizeof old, name);
    if (renameat(dfd, name, dfd, old) == 0) rmtree_at(dfd, old, 0);
    else if (errno != ENOENT) return -1;
    if (mkdirat(dfd, name, 0700) != 0) return -1;
    int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && fchmod(fd, mode) != 0) { close(fd); return -1; }
    return fd;
}

/* /tmp/schema-private/<name>/{tmp,var-tmp}, both 1777, made empty: the service
 * gets the host /tmp's filesystem and its limit, not a tmpfs of its own that
 * defaults to half the RAM. schema-private is root's and 0700, so no other
 * user reaches into it; one already there that isn't is moved aside. Reached
 * only through fds, bound through /proc/self/fd, so nothing in host /tmp can
 * redirect the bind. */
static int private_tmp_dirs(const char *name, int out[2]) {
    struct stat st;
    int tfd, pfd = -1, sfd = -1, ok = -1;
    if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        errno = EINVAL;
        return -1;
    }
    tfd = open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (tfd < 0) return -1;
    if (mkdirat(tfd, "schema-private", 0700) != 0 && errno != EEXIST) goto out;
    pfd = openat(tfd, "schema-private", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (pfd < 0 || fstat(pfd, &st) != 0 || st.st_uid != 0 || (st.st_mode & 077)) {
        char aside[96];
        if (pfd >= 0) close(pfd);
        aside_name(aside, sizeof aside, "schema-private.bad");
        if (renameat(tfd, "schema-private", tfd, aside) != 0 ||
            mkdirat(tfd, "schema-private", 0700) != 0)
            goto out;
        pfd = openat(tfd, "schema-private", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (pfd < 0) goto out;
    }
    sfd = fresh_dir(pfd, name, 0700);
    if (sfd < 0) goto out;
    out[0] = fresh_dir(sfd, "tmp", 01777);
    out[1] = out[0] < 0 ? -1 : fresh_dir(sfd, "var-tmp", 01777);
    if (out[1] < 0) { if (out[0] >= 0) close(out[0]); goto out; }
    ok = 0;
out:
    if (sfd >= 0) close(sfd);
    if (pfd >= 0) close(pfd);
    close(tfd);
    return ok;
}

static int bind_fd(int fd, const char *target) {
    char src[32];
    struct { uint64_t attr_set, attr_clr, propagation, userns_fd; } attr = { 0x2 | 0x4, 0, 0, 0 };
    struct stat st;
    snprintf(src, sizeof src, "/proc/self/fd/%d", fd);
    /* skip only a missing target: an unreachable source must not leave the
     * service on the shared host /tmp */
    if (stat(target, &st) != 0 && errno == ENOENT) return 0;
    if (mount(src, target, NULL, MS_BIND, NULL) != 0) return -1;
    /* nosuid + nodev added; a remount would also drop noexec and the rest
     * the host /tmp was mounted with */
    return (int)syscall(SYS_mount_setattr, AT_FDCWD, target, 0, &attr, sizeof(attr));
}

int apply_mount_ns(const char *name, uint8_t private_tmp, uint8_t protect_system,
                   uint8_t protect_home, const char *const *rw, int n_rw,
                   const char **step) {
    static const char *sys_paths[] = { "/usr", "/boot", "/efi", "/etc" };
    static const char *tmp_paths[] = { "/tmp", "/var/tmp" };
    static const char *home_paths[] = { "/home", "/root", "/run/user" };
    int i;

    if (!private_tmp && !protect_system && !protect_home) return 0;

    *step = "unshare";
    if (unshare(CLONE_NEWNS) != 0) return -1;
    *step = "propagation";
    if (mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL) != 0) return -1;

    if (private_tmp) {
        int fd[2];
        *step = "private_tmp";
        if (private_tmp_dirs(name, fd) == 0) {
            /* /var/tmp first: once /tmp is covered, a /tmp path means the new one */
            int r = bind_fd(fd[1], tmp_paths[1]) != 0 || bind_fd(fd[0], tmp_paths[0]) != 0 ? -1 : 0;
            close(fd[0]);
            close(fd[1]);
            if (r) return -1;
        } else {
            /* host /tmp not writable yet (read-only root early in boot), or
             * schema-private unusable: a tmpfs as before, but capped, not
             * half the RAM each */
            dprintf(2, "[schema-init] %s: private_tmp: /tmp/schema-private: %s; using a tmpfs capped at 10%%\n",
                    name, strerror(errno));
            for (i = 0; i < 2; i++)
                if (mount_tmpfs(tmp_paths[i], MS_NOSUID | MS_NODEV, "mode=1777,size=10%,nr_inodes=64k") != 0)
                    return -1;
        }
    }
    if (protect_system) {
        int n = protect_system == PROTECT_SYSTEM_FULL ? 4 : 3;
        *step = "protect_system";
        for (i = 0; i < n; i++)
            if (bind_attr(sys_paths[i], 1) != 0) return -1;
    }
    if (protect_home) {
        *step = "protect_home";
        for (i = 0; i < 3; i++)
            if (mount_tmpfs(home_paths[i],
                            MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC,
                            "mode=0755") != 0)
                return -1;
    }
    /* systemd's ReadWritePaths=: a fresh bind inherits the read-only flag of
     * the mount it sits in, so clear it on the new mount only */
    *step = "rw_path";
    for (i = 0; i < n_rw; i++)
        if (bind_attr(rw[i], 0) != 0) return -1;
    return 0;
}
