#ifndef UDEV_DB_H
#define UDEV_DB_H

#include "schema-udev.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
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

/* defined below; write_full/remove prune the tag index from the prior record */
static inline int udev_db_read_links_tags(const char *path,
        char links[][UE_VAL_MAX], int *nlink, int maxlink,
        char tags[][UE_KEY_MAX], int *ntag, int maxtag);

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

/* Real udevd maintains a tag index at <udev-root>/tags/<tag>/<device-id> that
 * sd-device's add_match_tag() reads — loginctl's seat-device tree and every
 * other tag-based enumeration. The per-record G:/Q: lines are invisible to it,
 * so the index must be written alongside the data record. The index root is the
 * "tags" sibling of the data dir (/run/udev/data -> /run/udev/tags). */
static inline int udev_db_tags_root(const char *base_dir, char *out, size_t outsz) {
    char parent[512];
    safe_copy(parent, base_dir, sizeof parent);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return -1;
    *slash = '\0';
    int w = snprintf(out, outsz, "%s/tags", parent);
    return (w > 0 && (size_t)w < outsz) ? 0 : -1;
}

static inline void udev_db_tag_index_set(const char *base_dir, const char *devid,
                                         const char *const *tags, int ntag) {
    char root[512];
    if (udev_db_tags_root(base_dir, root, sizeof root) != 0) return;
    for (int i = 0; i < ntag; i++) {
        if (!tags[i] || !tags[i][0]) continue;
        char dir[600];
        if ((size_t)snprintf(dir, sizeof dir, "%s/%s", root, tags[i]) >= sizeof dir) continue;
        if (udev_db_ensure_dir(dir) != 0) continue;
        char path[768];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, devid) >= sizeof path) continue;
        int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0444);
        if (fd >= 0) close(fd);
    }
}

static inline void udev_db_tag_index_clear(const char *base_dir, const char *devid,
                                           char tags[][UE_KEY_MAX], int ntag) {
    char root[512];
    if (udev_db_tags_root(base_dir, root, sizeof root) != 0) return;
    for (int i = 0; i < ntag; i++) {
        if (!tags[i][0]) continue;
        char path[768];
        if ((size_t)snprintf(path, sizeof path, "%s/%s/%s", root, tags[i], devid) >= sizeof path) continue;
        unlink(path);
    }
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
    char final[512], tmpl[512];
    if ((size_t)snprintf(final, sizeof final, "%s/%s", base_dir, name) >= sizeof final) return -1;

    /* snapshot the record's current tags before overwrite, so index entries for
     * tags it no longer carries get pruned (real udevd diffs old vs new). */
    char oldtags[64][UE_KEY_MAX]; int nold = 0;
    { char oldlinks[1][UE_VAL_MAX]; int nol = 0;
      udev_db_read_links_tags(final, oldlinks, &nol, 1, oldtags, &nold, 64); }

    char buf[8192];
    ssize_t len = udev_db_record_build_full(ev, kernel_n, symlinks, nsym,
                                            udev_db_now_usec(),
                                            tags, ntag, buf, sizeof buf);
    if (len <= 0) return -1;
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

    /* maintain the sd-device tag index: drop stale tags, add current ones */
    udev_db_tag_index_clear(base_dir, name, oldtags, nold);
    udev_db_tag_index_set(base_dir, name, tags, ntag);
    return 0;
}

static inline int udev_db_remove(const char *base_dir, const struct uevent *ev) {
    char name[128];
    if (udev_db_filename(ev, name, sizeof name) != 0) return -1;
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", base_dir, name) >= sizeof path) return -1;
    /* drop tag-index entries for this device before removing the record */
    char oldtags[64][UE_KEY_MAX]; int nold = 0;
    { char oldlinks[1][UE_VAL_MAX]; int nol = 0;
      udev_db_read_links_tags(path, oldlinks, &nol, 1, oldtags, &nold, 64); }
    udev_db_tag_index_clear(base_dir, name, oldtags, nold);
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
