#include "landlock.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/landlock.h>

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15)
#endif

#define FS_RO   (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | \
                 LANDLOCK_ACCESS_FS_READ_DIR)
#define FS_FILE (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | \
                 LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_TRUNCATE | \
                 LANDLOCK_ACCESS_FS_IOCTL_DEV)

int landlock_beneath(const char *path, const char *p) {
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') n--;
    if (strncmp(path, p, n) != 0) return 0;
    return path[n] == '\0' || path[n] == '/' || (n == 1 && p[0] == '/');
}

int apply_landlock(char *const *paths, uint32_t rw_mask, int n, const char **step) {
    struct landlock_ruleset_attr attr = { .handled_access_fs = (1ULL << 16) - 1 };
    int abi, rs, i;

    *step = "abi";
    abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0,
                       LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) return -1;
    if (abi < 2) attr.handled_access_fs &= ~LANDLOCK_ACCESS_FS_REFER;
    if (abi < 3) attr.handled_access_fs &= ~LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi < 5) attr.handled_access_fs &= ~LANDLOCK_ACCESS_FS_IOCTL_DEV;

    *step = "create_ruleset";
    rs = (int)syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0);
    if (rs < 0) return -1;

    for (i = 0; i < n; i++) {
        struct landlock_path_beneath_attr pb;
        struct stat st;
        int fd = open(paths[i], O_PATH | O_CLOEXEC);
        if (fd < 0) {
            int e = errno;
            if (e == ENOENT) continue;
            *step = paths[i];
            close(rs);
            errno = e;
            return -1;
        }
        pb.parent_fd = fd;
        pb.allowed_access = (rw_mask & (1U << i)) ? attr.handled_access_fs : FS_RO;
        if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode))
            pb.allowed_access &= FS_FILE;
        pb.allowed_access &= attr.handled_access_fs;
        if (syscall(SYS_landlock_add_rule, rs, LANDLOCK_RULE_PATH_BENEATH, &pb, 0) != 0) {
            int e = errno;
            *step = paths[i];
            close(fd);
            close(rs);
            errno = e;
            return -1;
        }
        close(fd);
    }

    *step = "restrict_self";
    if (syscall(SYS_landlock_restrict_self, rs, 0) != 0) {
        int e = errno;
        close(rs);
        errno = e;
        return -1;
    }
    close(rs);
    return 0;
}
