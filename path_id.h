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
    char tmp[PATH_ID_MAX];
    if (path[0]) snprintf(tmp, sizeof tmp, "%s-%s", comp, path);
    else         snprintf(tmp, sizeof tmp, "%s", comp);
    safe_copy(path, tmp, pathsz);
}

#endif /* PATH_ID_H */
