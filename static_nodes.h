#ifndef STATIC_NODES_H
#define STATIC_NODES_H

/* kmod-static-nodes parity: create the /dev nodes modules.devname declares so
 * the first open() autoloads the module (tun, uinput, vhost-net, ...). Every
 * entry point takes a root prefix ("" in PID 1) so tests run in a tmpdir. */

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>

struct sn_node { char devname[128]; char type; unsigned maj, min; };

/* modules.devname is root-owned, but PID 1 still refuses a name that would
 * land outside /dev. */
static inline int sn_path_ok(const char *rel) {
    if (rel[0] == '/' || rel[0] == '\0') return 0;
    for (const char *p = rel; (p = strstr(p, "..")); p += 2)
        if ((p == rel || p[-1] == '/') && (p[2] == '/' || p[2] == '\0')) return 0;
    return 1;
}

/* "<module> <devname> <c|b><major>:<minor>"; 1 parsed, 0 skip. */
static inline int sn_parse_devname_line(const char *line, struct sn_node *out) {
    char extra;
    if (line[0] == '#') return 0;
    if (sscanf(line, "%*s %127s %c%u:%u %c", out->devname, &out->type,
               &out->maj, &out->min, &extra) != 4) return 0;
    if (out->type != 'c' && out->type != 'b') return 0;
    return sn_path_ok(out->devname);
}

/* chmod after mkdir: a 0700 /dev/net under a strict umask would hide tun */
static inline void sn_mkdir(const char *path) {
    if (mkdir(path, 0755) == 0) chmod(path, 0755);
}

static inline void sn_mkparents(char *path) {
    char *slash = strrchr(path, '/');
    if (!slash) return;
    *slash = '\0';
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        sn_mkdir(path);
        *p = '/';
    }
    sn_mkdir(path);
    *slash = '/';
}

static inline int sn_create_nodes(const char *root, const char *devname_file,
                                  char *created, size_t created_sz) {
    created[0] = '\0';
    FILE *f = fopen(devname_file, "re");
    if (!f) return 0;
    int n = 0;
    size_t used = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        struct sn_node nd;
        if (!sn_parse_devname_line(line, &nd)) continue;
        char path[512];
        if ((size_t)snprintf(path, sizeof path, "%s/dev/%s", root, nd.devname) >= sizeof path)
            continue;
        sn_mkparents(path);
        mode_t m = (nd.type == 'c' ? S_IFCHR : S_IFBLK) | 0600;
        if (mknod(path, m, makedev(nd.maj, nd.min)) != 0) {
            if (errno != EEXIST)
                printf("[schema-init] static node %s: %s\n", path, strerror(errno));
            continue;
        }
        n++;
        int w = snprintf(created + used, created_sz - used, "%s%s", used ? " " : "", nd.devname);
        if (w > 0 && used + (size_t)w < created_sz) used += (size_t)w;
        else created[used] = '\0';
    }
    fclose(f);
    return n;
}

/* tmpfiles "z" line; empty user/group and has_mode=0 mean "-" (leave as is). */
struct sn_perm { char path[128]; int has_mode; mode_t mode; char user[64]; char group[64]; };

static inline int sn_parse_perm_line(const char *line, struct sn_perm *out) {
    char type[8], mode[16] = "-", user[64] = "-", group[64] = "-";
    if (sscanf(line, "%7s %127s %15s %63s %63s", type, out->path, mode, user, group) < 2)
        return 0;
    if (strcmp(type, "z") != 0) return 0;
    if (strncmp(out->path, "/dev/", 5) != 0 || !sn_path_ok(out->path + 5)) return 0;
    /* plain octal only; tmpfiles' ~ and : prefixes are not used by this file */
    out->has_mode = mode[0] && strlen(mode) <= 4 && strspn(mode, "01234567") == strlen(mode);
    out->mode = out->has_mode ? (mode_t)strtoul(mode, NULL, 8) : 0;
    snprintf(out->user, sizeof out->user, "%s", strcmp(user, "-") ? user : "");
    snprintf(out->group, sizeof out->group, "%s", strcmp(group, "-") ? group : "");
    return 1;
}

/* first directory holding the file wins */
static inline int sn_find_perm_file(const char *root, char *out, size_t sz) {
    /* systemd's order minus /run: PID 1 has just mounted it empty */
    static const char *dirs[] = { "/etc", "/usr/local/lib", "/usr/lib" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(out, sz, "%s%s/tmpfiles.d/static-nodes-permissions.conf", root, dirs[i]);
        if (access(out, F_OK) == 0) return 0;
    }
    return -1;
}

/* all digits -> numeric id, else a name lookup */
static inline int sn_resolve_id(const char *name, int is_group, unsigned *id) {
    char *end;
    errno = 0;
    unsigned long v = strtoul(name, &end, 10);
    if (*name && !*end && !errno && v < (unsigned long)(uid_t)-1) { *id = (unsigned)v; return 0; }
    if (is_group) { struct group *g = getgrnam(name); if (!g) return -1; *id = g->gr_gid; }
    else          { struct passwd *p = getpwnam(name); if (!p) return -1; *id = p->pw_uid; }
    return 0;
}

static inline void sn_apply_perms(const char *root, const char *perm_file) {
    FILE *f = fopen(perm_file, "re");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        struct sn_perm pm;
        if (!sn_parse_perm_line(line, &pm)) continue;
        char path[512];
        if ((size_t)snprintf(path, sizeof path, "%s%s", root, pm.path) >= sizeof path)
            continue;
        /* Same discipline as schema-udev's node_apply_perms: O_PATH doesn't open
         * the device (no autoload), O_NOFOLLOW refuses a symlink, and every
         * change goes through the fd's procfs alias so it lands on this inode. */
        int fd = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            if (errno != ENOENT && errno != ELOOP)
                printf("[schema-init] static node %s: %s\n", path, strerror(errno));
            continue;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode))) {
            close(fd);
            continue;
        }
        uid_t uid = st.st_uid;
        gid_t gid = st.st_gid;
        unsigned id;
        if (pm.user[0]) {
            if (sn_resolve_id(pm.user, 0, &id) == 0) uid = id;
            else printf("[schema-init] static node %s: unknown user '%s'\n", path, pm.user);
        }
        if (pm.group[0]) {
            if (sn_resolve_id(pm.group, 1, &id) == 0) gid = id;
            else printf("[schema-init] static node %s: unknown group '%s'\n", path, pm.group);
        }
        char fdpath[64];
        snprintf(fdpath, sizeof fdpath, "/proc/self/fd/%d", fd);
        if ((uid != st.st_uid || gid != st.st_gid) && chown(fdpath, uid, gid) != 0)
            printf("[schema-init] static node %s: %s\n", path, strerror(errno));
        if (pm.has_mode && chmod(fdpath, pm.mode & 07777) != 0)
            printf("[schema-init] static node %s: %s\n", path, strerror(errno));
        close(fd);
    }
    fclose(f);
}

/* /lib is a symlink to usr/lib on merged-/usr systems; the fallback covers a
 * box where it isn't. */
static inline int sn_find_devname_file(const char *root, const char *release,
                                       char *out, size_t sz) {
    static const char *dirs[] = { "/lib/modules", "/usr/lib/modules" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(out, sz, "%s%s/%s/modules.devname", root, dirs[i], release);
        if (access(out, F_OK) == 0) return 0;
    }
    return -1;
}

/* First boot only, after mount_pseudo(): devtmpfs and /proc are mounted. */
static inline void static_nodes_boot(void) {
    struct utsname u;
    struct statfs fs;
    char path[512], created[512];
    /* devtmpfs reports tmpfs (or ramfs without CONFIG_TMPFS). Anything else
     * means the mount failed and /dev is a directory on the root fs, where
     * nodes would persist across boots. */
    if (statfs("/dev", &fs) != 0 ||
        (fs.f_type != TMPFS_MAGIC && fs.f_type != RAMFS_MAGIC)) return;
    if (uname(&u) == 0 && sn_find_devname_file("", u.release, path, sizeof path) == 0) {
        int n = sn_create_nodes("", path, created, sizeof created);
        if (n > 0) printf("[schema-init] static nodes: created %d (%s)\n", n, created);
    }
    if (sn_find_perm_file("", path, sizeof path) == 0) sn_apply_perms("", path);
}

#endif
