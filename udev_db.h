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

#define SCHEMA_UDEV_DB_DIR "/run/schema-udev/data"   /* OUR shadow dir */
#define UDEV_DB_DIR        "/run/udev/data"          /* udevd's real dir (read-only) */

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

static inline ssize_t udev_db_record_build(const struct uevent *ev, char *buf, size_t bufsz) {
    int w = snprintf(buf, bufsz, "V:1\n");
    if (w < 0 || (size_t)w >= bufsz) return -1;
    size_t used = (size_t)w;
    for (int i = 0; i < ev->n; i++) {
        w = snprintf(buf + used, bufsz - used, "E:%s=%s\n", ev->key[i], ev->val[i]);
        if (w < 0 || (size_t)w >= bufsz - used) return -1;
        used += (size_t)w;
    }
    return (ssize_t)used;
}

static inline int udev_db_write(const char *base_dir, const struct uevent *ev) {
    char name[128];
    if (udev_db_filename(ev, name, sizeof name) != 0) return -1;
    struct stat st;
    if (stat(base_dir, &st) != 0 && mkdir(base_dir, 0755) != 0 && errno != EEXIST) return -1;
    char path[512], buf[8192];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", base_dir, name) >= sizeof path) return -1;
    ssize_t len = udev_db_record_build(ev, buf, sizeof buf);
    if (len < 0) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fwrite(buf, 1, (size_t)len, f);
    fclose(f);
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

#endif /* UDEV_DB_H */
