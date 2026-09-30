#ifndef DISK_LINKS_H
#define DISK_LINKS_H

#include "schema-udev.h"
#include "udev_db.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <ftw.h>

#define SCHEMA_DISK_DIR "/dev/schema/disk"

struct disk_link { const char *tree; char name[UE_VAL_MAX]; };

static inline int disk_links_derive(const struct uevent *ev,
                                    struct disk_link *out, int max) {
    int n = 0;
    const char *v;
    struct { const char *tree; const char *key; const char *fallback; } simple[] = {
        { "by-uuid",      "ID_FS_UUID_ENC",     "ID_FS_UUID" },
        { "by-label",     "ID_FS_LABEL_ENC",    NULL },
        { "by-partuuid",  "ID_PART_ENTRY_UUID", NULL },
        { "by-partlabel", "ID_PART_ENTRY_NAME", NULL },
    };
    for (size_t i = 0; i < sizeof simple / sizeof simple[0] && n < max; i++) {
        v = uevent_get(ev, simple[i].key);
        if ((!v || !v[0]) && simple[i].fallback) v = uevent_get(ev, simple[i].fallback);
        if (v && v[0]) {
            out[n].tree = simple[i].tree;
            safe_copy(out[n].name, v, sizeof out[n].name);
            n++;
        }
    }
    const char *devtype = uevent_get(ev, "DEVTYPE");
    const char *partn   = uevent_get(ev, "PARTN");
    int is_part = devtype && strcmp(devtype, "partition") == 0;
    struct { const char *tree; const char *key; } suff[] = {
        { "by-path",    "ID_PATH" },
        { "by-diskseq", "DISKSEQ" },
    };
    for (size_t i = 0; i < sizeof suff / sizeof suff[0] && n < max; i++) {
        v = uevent_get(ev, suff[i].key);
        if (!v || !v[0]) continue;
        out[n].tree = suff[i].tree;
        if (is_part && partn && partn[0])
            snprintf(out[n].name, sizeof out[n].name, "%s-part%s", v, partn);
        else
            safe_copy(out[n].name, v, sizeof out[n].name);
        n++;
    }
    return n;
}

static inline int dl_mkdir_p(const char *path) {
    char tmp[512];
    safe_copy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static inline int dl_link_one(const char *base_dir, const char *tree,
                              const char *name, const char *devname) {
    char treedir[768];
    if ((size_t)snprintf(treedir, sizeof treedir, "%s/%s", base_dir, tree) >= sizeof treedir)
        return -1;
    if (dl_mkdir_p(treedir) != 0) return -1;

    char target[600];
    /* One ".." per path component of base_dir hops from the link's directory
       (base_dir/<tree>/) up to /dev, where the real block node lives (kernel
       devtmpfs, in live and shadow alike). A fixed "../../../" only matched the
       3-component shadow base (/dev/schema/disk); the live base (/dev/disk) is
       one component shallower, so it overshot /dev to "/" and every by-* link
       dangled (/dev/disk/by-uuid/X -> ../../../sda1 = /sda1). */
    size_t ups = 0;
    for (const char *p = base_dir; *p; p++)
        if (*p == '/' && p[1] && p[1] != '/') ups++;
    char prefix[128];
    size_t off = 0;
    for (size_t u = 0; u < ups && off + 3 < sizeof prefix; u++) {
        memcpy(prefix + off, "../", 3);
        off += 3;
    }
    prefix[off] = '\0';
    if ((size_t)snprintf(target, sizeof target, "%s%s", prefix, devname) >= sizeof target)
        return -1;

    char final[1024], tmp[1024];
    if ((size_t)snprintf(final, sizeof final, "%s/%s", treedir, name) >= sizeof final)
        return -1;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s/.%s.tmp.%d", treedir, name, (int)getpid()) >= sizeof tmp)
        return -1;

    unlink(tmp);
    if (symlink(target, tmp) != 0) return -1;
    if (rename(tmp, final) != 0) { unlink(tmp); return -1; }
    return 0;
}

static inline int disk_links_apply(const char *base_dir, const struct uevent *ev) {
    const char *devname = uevent_get(ev, "DEVNAME");
    if (!devname || !devname[0]) return -1;
    const char *slash = strrchr(devname, '/');
    if (slash) devname = slash + 1;

    struct disk_link links[8];
    int n = disk_links_derive(ev, links, 8);
    for (int i = 0; i < n; i++)
        dl_link_one(base_dir, links[i].tree, links[i].name, devname);
    return 0;
}

static inline int disk_links_gc(const char *base_dir, const char *db_dir,
                                const struct uevent *ev) {
    char name[128];
    if (udev_db_filename(ev, name, sizeof name) != 0) return -1;
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", db_dir, name) >= sizeof path) return -1;

    struct uevent merged; memset(&merged, 0, sizeof merged);
    udev_db_read_eprops(path, &merged);            /* derived props; ok if record absent */

    const char *k;
    const char *graft[] = { "DEVTYPE", "DISKSEQ", "PARTN" };
    for (size_t i = 0; i < sizeof graft / sizeof graft[0]; i++) {
        if ((k = uevent_get(ev, graft[i])) && merged.n < UE_MAX_KEYS) {
            safe_copy(merged.key[merged.n], graft[i], UE_KEY_MAX);
            safe_copy(merged.val[merged.n], k, UE_VAL_MAX);
            merged.n++;
        }
    }

    struct disk_link links[8];
    int n = disk_links_derive(&merged, links, 8);
    for (int i = 0; i < n; i++) {
        char lp[1024];
        if ((size_t)snprintf(lp, sizeof lp, "%s/%s/%s",
                             base_dir, links[i].tree, links[i].name) >= sizeof lp)
            continue;
        if (unlink(lp) != 0 && errno != ENOENT) { /* best-effort */ }
    }
    return 0;
}

/* Rule SYMLINK+= names (the S: lines of a db record) are relative to /dev and
   may nest (disk/by-id/X, serial/by-id/X, input/by-id/X). Reject anything that
   could escape dev_root. */
static inline int rule_link_name_ok(const char *name) {
    if (!name || !name[0] || name[0] == '/') return 0;
    for (const char *p = name; ; ) {
        const char *s = strchr(p, '/');
        size_t l = s ? (size_t)(s - p) : strlen(p);
        if (l == 0 || (l == 1 && p[0] == '.') || (l == 2 && p[0] == '.' && p[1] == '.'))
            return 0;
        if (!s) return 1;
        p = s + 1;
    }
}

/* udev's link target: one ".." per directory level of NAME, then DEVNAME
   (itself relative to /dev, e.g. "sda" or "input/event3"). */
static inline int rule_link_target(const char *name, const char *devname,
                                   char *out, size_t outsz) {
    size_t off = 0;
    for (const char *p = name; *p; p++) {
        if (*p != '/') continue;
        if (off + 3 >= outsz) return -1;
        memcpy(out + off, "../", 3);
        off += 3;
    }
    if ((size_t)snprintf(out + off, outsz - off, "%s", devname) >= outsz - off) return -1;
    return 0;
}

static inline int rule_link_one(const char *dev_root, const char *name, const char *devname) {
    if (!rule_link_name_ok(name)) return -1;
    char final[1024], dir[1024], tmp[1100], target[600];
    if ((size_t)snprintf(final, sizeof final, "%s/%s", dev_root, name) >= sizeof final) return -1;
    if (rule_link_target(name, devname, target, sizeof target) != 0) return -1;
    safe_copy(dir, final, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = '\0';
    if (dl_mkdir_p(dir) != 0) return -1;
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp.%d", dir, slash + 1, (int)getpid());
    unlink(tmp);
    if (symlink(target, tmp) != 0) return -1;
    if (rename(tmp, final) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Drop a link only while it still points at DEVNAME: another device may have
   claimed the same name since. */
static inline void rule_link_clear(const char *dev_root, const char *name, const char *devname) {
    if (!rule_link_name_ok(name)) return;
    char path[1024], want[600], have[600];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", dev_root, name) >= sizeof path) return;
    if (rule_link_target(name, devname, want, sizeof want) != 0) return;
    ssize_t l = readlink(path, have, sizeof have - 1);
    if (l < 0) return;
    have[l] = '\0';
    if (strcmp(have, want) == 0) unlink(path);
}

/* Converge /dev on a device's rule links: clear the ones its previous record
   carried that the new set drops, then (re)create the new set. A remove is an
   update to the empty set. */
static inline void rule_links_update(const char *dev_root, const char *devname,
                                     char old[][UE_VAL_MAX], int nold,
                                     const char *const *links, int nlinks) {
    if (!devname || !devname[0]) return;
    for (int i = 0; i < nold; i++) {
        int kept = 0;
        for (int j = 0; j < nlinks && !kept; j++)
            kept = strcmp(old[i], links[j]) == 0;
        if (!kept) rule_link_clear(dev_root, old[i], devname);
    }
    for (int j = 0; j < nlinks; j++)
        rule_link_one(dev_root, links[j], devname);
}

static inline int dl_rm_cb(const char *p, const struct stat *sb,
                           int type, struct FTW *ftw) {
    (void)sb; (void)type; (void)ftw;
    remove(p);
    return 0;
}

static inline void disk_links_wipe(const char *base_dir) {
    nftw(base_dir, dl_rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

#endif /* DISK_LINKS_H */
