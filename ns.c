#include "ns.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/syscall.h>

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

int apply_mount_ns(uint8_t private_tmp, uint8_t protect_system,
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
        *step = "private_tmp";
        for (i = 0; i < 2; i++)
            if (mount_tmpfs(tmp_paths[i], MS_NOSUID | MS_NODEV, "mode=1777") != 0)
                return -1;
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
