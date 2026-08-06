#ifndef PATH_ID_H
#define PATH_ID_H

#include "schema-udev.h"
#include <limits.h>

#define PATH_ID_MAX 512

static inline int path_id_tag(const char *id_path, char *out, size_t outsz) {
    size_t i = 0;
    for (; id_path[i]; i++) {
        if (i + 1 >= outsz) return -1;
        char c = id_path[i];
        out[i] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-') ? c : '_';
    }
    if (i >= outsz) return -1;
    out[i] = '\0';
    return 0;
}

static inline int pi_subsystem(const char *devdir, char *out, size_t outsz) {
    char link[PATH_MAX], target[PATH_MAX];
    if ((size_t)snprintf(link, sizeof link, "%s/subsystem", devdir) >= sizeof link) return -1;
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n <= 0) return -1;
    target[n] = '\0';
    char *b = strrchr(target, '/');
    safe_copy(out, b ? b + 1 : target, outsz);
    return 0;
}

static inline int pi_sysattr(const char *devdir, const char *attr, char *out, size_t outsz) {
    char p[PATH_MAX];
    if ((size_t)snprintf(p, sizeof p, "%s/%s", devdir, attr) >= sizeof p) return -1;
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    if (!fgets(out, (int)outsz, f)) { fclose(f); return -1; }
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

static inline const char *pi_base(const char *dir) {
    const char *b = strrchr(dir, '/');
    return b ? b + 1 : dir;
}

static inline int pi_parent(char *cur) {
    char *b = strrchr(cur, '/');
    if (!b || b == cur) return -1;
    *b = '\0';
    return 0;
}

static inline void pi_prepend(char *path, size_t pathsz, const char *comp) {
    char tmp[PATH_ID_MAX * 2];
    if (path[0]) snprintf(tmp, sizeof tmp, "%s-%s", comp, path);
    else         snprintf(tmp, sizeof tmp, "%s", comp);
    safe_copy(path, tmp, pathsz);
}

static inline int pi_handle_usb(const char *leafdir, char *cur, size_t cursz,
                                char *path, size_t pathsz) {
    (void)leafdir; (void)cur; (void)cursz; (void)path; (void)pathsz;
    return 0;   /* stub: filled in Task 3 */
}
static inline int pi_handle_scsi(const char *leafdir, char *cur, size_t cursz,
                                 char *path, size_t pathsz) {
    (void)leafdir; (void)cur; (void)cursz; (void)path; (void)pathsz;
    return 0;   /* stub: filled in Task 5 */
}
static inline int pi_handle_nvme(const char *leafdir, char *cur, size_t cursz,
                                 char *path, size_t pathsz) {
    (void)leafdir; (void)cur; (void)cursz; (void)path; (void)pathsz;
    return 0;   /* stub: filled in Task 4 */
}

static inline ssize_t path_id_build(const char *sysroot, const char *devpath,
                                    char *out, size_t outsz) {
    char devroot[PATH_MAX], cur[PATH_MAX], leafdir[PATH_MAX];
    if ((size_t)snprintf(devroot, sizeof devroot, "%s/devices", sysroot) >= sizeof devroot)
        return -1;
    if ((size_t)snprintf(cur, sizeof cur, "%s%s", sysroot, devpath) >= sizeof cur)
        return -1;
    safe_copy(leafdir, cur, sizeof leafdir);

    size_t rootlen = strlen(devroot);
    char path[PATH_ID_MAX] = "";
    int anchored = 0;
    char comp[PATH_ID_MAX], sub[128];

    while (strlen(cur) > rootlen) {
        if (pi_subsystem(cur, sub, sizeof sub) != 0) {
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "pci") == 0) {
            snprintf(comp, sizeof comp, "pci-%s", pi_base(cur));
            pi_prepend(path, sizeof path, comp);
            anchored = 1;
            /* skip all pci ancestors (bridges) */
            for (;;) {
                if (pi_parent(cur) != 0 || strlen(cur) <= rootlen) break;
                if (pi_subsystem(cur, sub, sizeof sub) != 0 || strcmp(sub, "pci") != 0) break;
            }
            continue;
        }
        if (strcmp(sub, "platform") == 0) {
            snprintf(comp, sizeof comp, "platform-%s", pi_base(cur));
            pi_prepend(path, sizeof path, comp);
            anchored = 1;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "usb") == 0) {
            if (pi_handle_usb(leafdir, cur, sizeof cur, path, sizeof path)) continue;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "scsi") == 0) {
            if (pi_handle_scsi(leafdir, cur, sizeof cur, path, sizeof path)) continue;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "nvme") == 0) {
            if (pi_handle_nvme(leafdir, cur, sizeof cur, path, sizeof path)) continue;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (pi_parent(cur) != 0) break;
    }

    if (!anchored || path[0] == '\0') return -1;
    size_t plen = strlen(path);
    if (plen + 1 > outsz) return -1;
    memcpy(out, path, plen + 1);
    return (ssize_t)plen;
}

#endif /* PATH_ID_H */
