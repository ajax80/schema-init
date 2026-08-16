#ifndef UDEV_DB_H
#define UDEV_DB_H

#include "schema-udev.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* udev's USEC_INITIALIZED: monotonic microseconds when the device record is
 * committed. Consumers (libudev is_initialized, logind/udisks settle) gate on
 * the I: line's presence, so every committed record must carry one. */
static inline long long udev_db_now_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 1;
    long long u = (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
    return u > 0 ? u : 1;
}

#define SCHEMA_UDEV_DB_DIR "/run/schema-udev/data"   /* OUR shadow dir */
#define UDEV_DB_DIR        "/run/udev/data"          /* udevd's real dir (read-only) */
#define SCHEMA_UDEV_RULES_DIR "/run/schema-udev/rules-data"   /* R5 interpreter shadow */

static inline int udev_db_filename(const struct uevent *ev, char *out, size_t outsz) {
    const char *sub = uevent_get(ev, "SUBSYSTEM");
    const char *maj = uevent_get(ev, "MAJOR");
    const char *min = uevent_get(ev, "MINOR");
    const char *ifidx = uevent_get(ev, "IFINDEX");
    const char *devpath = uevent_get(ev, "DEVPATH");
    int w = -1;
    if (sub && strcmp(sub, "net") == 0 && ifidx)
        w = snprintf(out, outsz, "n%s", ifidx);
    else if (maj && min)
        w = snprintf(out, outsz, "%c%s:%s",
                     (sub && strcmp(sub, "block") == 0) ? 'b' : 'c', maj, min);
    else if (sub && devpath) {
        const char *slash = strrchr(devpath, '/');
        w = snprintf(out, outsz, "+%s:%s", sub, slash ? slash + 1 : devpath);
    } else
        return -1;
    return (w > 0 && (size_t)w < outsz) ? 0 : -1;
}

static inline ssize_t udev_db_record_build(const struct uevent *ev, int kernel_n,
                                           char *buf, size_t bufsz) {
    size_t used = 0;
    for (int i = kernel_n; i < ev->n; i++) {
        if (!ev->key[i][0] || !ev->val[i][0]) continue;
        if (ev->key[i][0] == '.') continue;   /* private prop: never persisted (matches udev) */
        int w = snprintf(buf + used, bufsz - used, "E:%s=%s\n", ev->key[i], ev->val[i]);
        if (w < 0 || (size_t)w >= bufsz - used) return -1;
        used += (size_t)w;
    }
    int w = snprintf(buf + used, bufsz - used, "V:1\n");
    if (w < 0 || (size_t)w >= bufsz - used) return -1;
    used += (size_t)w;
    return (ssize_t)used;
}

/* Full udev-db record: S: symlinks, I: init usec, E: derived props, G:/Q: tags,
 * V: version last. Byte-order matches real /run/udev/data records. */
static inline ssize_t udev_db_record_build_full(const struct uevent *ev, int kernel_n,
                                                const char *const *symlinks, int nsym,
                                                long long usec_init,
                                                const char *const *tags, int ntag,
                                                char *buf, size_t bufsz) {
    size_t used = 0;
    int w;
    for (int i = 0; i < nsym; i++) {
        if (!symlinks[i] || !symlinks[i][0]) continue;
        w = snprintf(buf + used, bufsz - used, "S:%s\n", symlinks[i]);
        if (w < 0 || (size_t)w >= bufsz - used) return -1;
        used += (size_t)w;
    }
    if (usec_init > 0) {
        w = snprintf(buf + used, bufsz - used, "I:%lld\n", usec_init);
        if (w < 0 || (size_t)w >= bufsz - used) return -1;
        used += (size_t)w;
    }
    for (int i = kernel_n; i < ev->n; i++) {
        if (!ev->key[i][0] || !ev->val[i][0]) continue;
        if (ev->key[i][0] == '.') continue;   /* private prop: never persisted (matches udev) */
        w = snprintf(buf + used, bufsz - used, "E:%s=%s\n", ev->key[i], ev->val[i]);
        if (w < 0 || (size_t)w >= bufsz - used) return -1;
        used += (size_t)w;
    }
    for (int i = 0; i < ntag; i++) {
        if (!tags[i] || !tags[i][0]) continue;
        w = snprintf(buf + used, bufsz - used, "G:%s\n", tags[i]);
        if (w < 0 || (size_t)w >= bufsz - used) return -1;
        used += (size_t)w;
    }
    for (int i = 0; i < ntag; i++) {
        if (!tags[i] || !tags[i][0]) continue;
        w = snprintf(buf + used, bufsz - used, "Q:%s\n", tags[i]);
        if (w < 0 || (size_t)w >= bufsz - used) return -1;
        used += (size_t)w;
    }
    w = snprintf(buf + used, bufsz - used, "V:1\n");
    if (w < 0 || (size_t)w >= bufsz - used) return -1;
    used += (size_t)w;
    return (ssize_t)used;
}

static inline int udev_db_ensure_dir(const char *d) {
    if (mkdir(d, 0755) == 0 || errno == EEXIST) return 0;
    if (errno != ENOENT) return -1;
    char parent[512];
    safe_copy(parent, d, sizeof parent);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return -1;
    *slash = '\0';
    if (udev_db_ensure_dir(parent) != 0) return -1;
    return (mkdir(d, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static inline int udev_db_write(const char *base_dir, const struct uevent *ev, int kernel_n) {
    char name[128];
    if (udev_db_filename(ev, name, sizeof name) != 0) return -1;
    if (udev_db_ensure_dir(base_dir) != 0) return -1;
    char buf[8192];
    ssize_t len = udev_db_record_build(ev, kernel_n, buf, sizeof buf);
    if (len <= 4) return 0;   /* no derived properties -> don't write 4-byte V:1-only file */
    char final[512], tmpl[512];
    if ((size_t)snprintf(final, sizeof final, "%s/%s", base_dir, name) >= sizeof final) return -1;
    if ((size_t)snprintf(tmpl, sizeof tmpl, "%s/.dbXXXXXX", base_dir) >= sizeof tmpl) return -1;
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    if (fchmod(fd, 0644) != 0) { close(fd); unlink(tmpl); return -1; }
    ssize_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, (size_t)(len - off));
        if (w < 0) { close(fd); unlink(tmpl); return -1; }
        off += w;
    }
    if (close(fd) != 0) { unlink(tmpl); return -1; }
    if (rename(tmpl, final) != 0) { unlink(tmpl); return -1; }
    return 0;
}

static inline int udev_db_write_full(const char *base_dir, const struct uevent *ev,
                                     int kernel_n,
                                     const char *const *symlinks, int nsym,
                                     const char *const *tags, int ntag) {
    char name[128];
    if (udev_db_filename(ev, name, sizeof name) != 0) return -1;
    if (udev_db_ensure_dir(base_dir) != 0) return -1;
    char buf[8192];
    ssize_t len = udev_db_record_build_full(ev, kernel_n, symlinks, nsym,
                                            udev_db_now_usec(),
                                            tags, ntag, buf, sizeof buf);
    if (len <= 0) return -1;
    char final[512], tmpl[512];
    if ((size_t)snprintf(final, sizeof final, "%s/%s", base_dir, name) >= sizeof final) return -1;
    if ((size_t)snprintf(tmpl, sizeof tmpl, "%s/.dbXXXXXX", base_dir) >= sizeof tmpl) return -1;
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    if (fchmod(fd, 0644) != 0) { close(fd); unlink(tmpl); return -1; }
    ssize_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, (size_t)(len - off));
        if (w < 0) { close(fd); unlink(tmpl); return -1; }
        off += w;
    }
    if (close(fd) != 0) { unlink(tmpl); return -1; }
    if (rename(tmpl, final) != 0) { unlink(tmpl); return -1; }
    return 0;
}

static inline int udev_db_remove(const char *base_dir, const struct uevent *ev) {
    char name[128];
    if (udev_db_filename(ev, name, sizeof name) != 0) return -1;
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", base_dir, name) >= sizeof path) return -1;
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}

static inline int udev_db_read_eprops(const char *path, struct uevent *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(out, 0, sizeof *out);
    char line[1024];
    while (fgets(line, sizeof line, f) && out->n < UE_MAX_KEYS) {
        if (line[0] != 'E' || line[1] != ':') continue;
        char *kv = line + 2;
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';
        safe_copy(out->key[out->n], kv, UE_KEY_MAX);
        safe_copy(out->val[out->n], val, UE_VAL_MAX);
        out->n++;
    }
    fclose(f);
    return 0;
}

static inline int udev_db_read_links_tags(const char *path,
        char links[][UE_VAL_MAX], int *nlink, int maxlink,
        char tags[][UE_KEY_MAX], int *ntag, int maxtag) {
    *nlink = 0; *ntag = 0;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == 'S' && line[1] == ':' && *nlink < maxlink)
            safe_copy(links[(*nlink)++], line + 2, UE_VAL_MAX);
        else if (line[0] == 'G' && line[1] == ':' && *ntag < maxtag)
            safe_copy(tags[(*ntag)++], line + 2, UE_KEY_MAX);
    }
    fclose(f);
    return 0;
}

static inline int udev_db_parse_eprops(const char *text, struct uevent *out) {
    out->n = 0;
    for (const char *p = text; *p; ) {
        const char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
        if (p[0] == 'E' && p[1] == ':' && out->n < UE_MAX_KEYS) {
            const char *kv = p + 2;
            const char *eq = memchr(kv, '=', linelen - 2);
            if (eq) {
                char k[UE_KEY_MAX], v[UE_VAL_MAX];
                size_t kl = (size_t)(eq - kv); if (kl >= UE_KEY_MAX) kl = UE_KEY_MAX - 1;
                size_t vl = (size_t)(p + linelen - (eq + 1)); if (vl >= UE_VAL_MAX) vl = UE_VAL_MAX - 1;
                memcpy(k, kv, kl); k[kl] = '\0';
                memcpy(v, eq + 1, vl); v[vl] = '\0';
                safe_copy(out->key[out->n], k, UE_KEY_MAX);
                safe_copy(out->val[out->n], v, UE_VAL_MAX);
                out->n++;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

#endif /* UDEV_DB_H */
