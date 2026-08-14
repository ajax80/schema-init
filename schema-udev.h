#ifndef SCHEMA_UDEV_H
#define SCHEMA_UDEV_H

#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdint.h>
#include <endian.h>

#define UE_MAX_KEYS 128
#define UE_KEY_MAX  64
#define UE_VAL_MAX  512

struct uevent {
    char key[UE_MAX_KEYS][UE_KEY_MAX];
    char val[UE_MAX_KEYS][UE_VAL_MAX];
    int  n;
};

static inline const char *uevent_get(const struct uevent *ev, const char *key) {
    for (int j = 0; j < ev->n; j++)
        if (strcmp(ev->key[j], key) == 0) return ev->val[j];
    return NULL;
}

/* Parse a raw kernel netlink uevent buffer:
 *   "action@devpath\0KEY=VALUE\0KEY=VALUE\0..."
 * The leading action@devpath record is skipped (redundant with ACTION=/DEVPATH=).
 * Returns 0 if >=1 KEY=VALUE parsed and ACTION present, -1 on malformed. */
static inline int uevent_parse(const char *buf, size_t len, struct uevent *ev) {
    ev->n = 0;
    size_t i = 0;
    while (i < len && buf[i] != '\0') i++;   /* skip header record */
    if (i >= len) return -1;                 /* no NUL -> not a uevent */
    i++;
    while (i < len && ev->n < UE_MAX_KEYS) {
        const char *rec = buf + i;
        size_t rl = strnlen(rec, len - i);
        if (rl == 0) { i++; continue; }
        const char *eq = memchr(rec, '=', rl);
        if (eq) {
            size_t klen = (size_t)(eq - rec);
            size_t vlen = rl - klen - 1;
            if (klen > 0 && klen < UE_KEY_MAX && vlen < UE_VAL_MAX) {
                memcpy(ev->key[ev->n], rec, klen); ev->key[ev->n][klen] = '\0';
                memcpy(ev->val[ev->n], eq + 1, vlen); ev->val[ev->n][vlen] = '\0';
                ev->n++;
            }
        }
        i += rl + 1;
    }
    return uevent_get(ev, "ACTION") ? 0 : -1;
}

#define RULE_MAX_MATCH 8
#define RULE_HOOK_MAX  512
#define RULE_NAME_MAX  64

struct dev_rule {
    char name[RULE_NAME_MAX];
    char mkey[RULE_MAX_MATCH][UE_KEY_MAX];
    char mpat[RULE_MAX_MATCH][UE_VAL_MAX];
    int  nmatch;
    char symlink[64];
    char on_add[RULE_HOOK_MAX];
    char on_remove[RULE_HOOK_MAX];
};

static inline int dev_rule_set(struct dev_rule *r, const char *key, const char *val) {
    if (strcmp(key, "name") == 0) {
        snprintf(r->name, sizeof r->name, "%s", val);
    } else if (strcmp(key, "symlink") == 0) {
        size_t len = strlen(val);
        if (len == 0 || len >= 64 || strchr(val, '/') || strstr(val, "..")) return -1;
        snprintf(r->symlink, sizeof r->symlink, "%s", val);
    } else if (strcmp(key, "on_add") == 0) {
        snprintf(r->on_add, sizeof r->on_add, "%s", val);
    } else if (strcmp(key, "on_remove") == 0) {
        snprintf(r->on_remove, sizeof r->on_remove, "%s", val);
    } else if (strncmp(key, "match_", 6) == 0) {
        const char *sub = key + 6;
        if (*sub == '\0' || r->nmatch >= RULE_MAX_MATCH) return -1;
        int k = r->nmatch;
        size_t z;
        for (z = 0; sub[z] && z < UE_KEY_MAX - 1; z++)
            r->mkey[k][z] = (char)toupper((unsigned char)sub[z]);
        r->mkey[k][z] = '\0';
        snprintf(r->mpat[k], sizeof r->mpat[k], "%s", val);
        r->nmatch++;
    } else {
        return -1;
    }
    return 0;
}

static inline int dev_rule_match(const struct dev_rule *r, const struct uevent *ev) {
    if (r->nmatch == 0) return 0;
    for (int k = 0; k < r->nmatch; k++) {
        const char *v = uevent_get(ev, r->mkey[k]);
        if (!v || fnmatch(r->mpat[k], v, 0) != 0) return 0;
    }
    return 1;
}

#define MAX_RULES 64

static inline int dev_rule_load_file(const char *path, struct dev_rule *r) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(r, 0, sizeof *r);
    char line[512];
    int nset = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';
        if (dev_rule_set(r, p, val) == 0)
            nset++;
        else
            fprintf(stderr, "[schema-udev] %s: ignoring unknown key '%s'\n", path, p);
    }
    fclose(f);
    return nset > 0 ? 0 : 1;   /* 1 = file had no rule keys (all comments/blank) */
}

static inline int dev_is_dotdev(const struct dirent *d) {
    const char *n = d->d_name;
    size_t l = strlen(n);
    return l > 4 && strcmp(n + l - 4, ".dev") == 0;
}

static inline int dev_rules_load_dir(const char *dir, struct dev_rule *rules, int max) {
    struct dirent **names = NULL;
    int nf = scandir(dir, &names, dev_is_dotdev, alphasort);
    if (nf < 0) return 0;
    int n = 0;
    for (int i = 0; i < nf; i++) {
        if (n < max) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s", dir, names[i]->d_name);
            if (dev_rule_load_file(path, &rules[n]) == 0) n++;
        }
        free(names[i]);
    }
    free(names);
    return n;
}

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#define SCHEMA_DEV_DIR "/dev/schema"

/* Boot-readiness marker: written once after the initial coldplug completes so
 * schema-init's udevd.svc ready_path can gate services that depend on the
 * device manager (network-up, etc). A dedicated path, not systemd's
 * /run/udev/control socket, to avoid confusing libudev/udevadm clients. */
#define SCHEMA_UDEV_READY_DIR  "/run/schema-udev"
#define SCHEMA_UDEV_READY      "/run/schema-udev/ready"

static inline int udev_signal_ready_at(const char *dir) {
    char path[PATH_MAX];
    mkdir(dir, 0755);   /* ok if it already exists */
    if ((size_t)snprintf(path, sizeof path, "%s/ready", dir) >= sizeof path) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

static inline int udev_signal_ready(void) {
    return udev_signal_ready_at(SCHEMA_UDEV_READY_DIR);
}

static inline int symlink_apply(const char *base_dir, const char *name, const char *devname) {
    if (!base_dir || !name || !devname || name[0] == '\0') return -1;
    
    struct stat st;
    if (stat(base_dir, &st) != 0) {
        if (mkdir(base_dir, 0755) != 0 && errno != EEXIST) return -1;
    }

    char target[512];
    if (devname[0] == '/') snprintf(target, sizeof target, "%s", devname);
    else snprintf(target, sizeof target, "/dev/%s", devname);

    char tmppath[512], finalpath[512];
    snprintf(tmppath, sizeof tmppath, "%s/.%s.tmp.%d", base_dir, name, (int)getpid());
    snprintf(finalpath, sizeof finalpath, "%s/%s", base_dir, name);

    unlink(tmppath);
    if (symlink(target, tmppath) != 0) return -1;
    if (rename(tmppath, finalpath) != 0) {
        unlink(tmppath);
        return -1;
    }
    return 0;
}

static inline int symlink_clear(const char *base_dir, const char *name) {
    if (!base_dir || !name || name[0] == '\0') return -1;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", base_dir, name);
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}

static inline void safe_copy(char *dst, const char *src, size_t maxlen) {
    size_t l = strlen(src);
    if (l >= maxlen) l = maxlen - 1;
    memcpy(dst, src, l);
    dst[l] = '\0';
}

/* Synthesize a struct uevent from a sysfs device directory D (which contains a uevent file).
 * sysroot is the sysfs root path (usually "/sys", or a /tmp test root).
 * Strips sysroot from dirpath to yield DEVPATH starting with "/devices/". */
static inline int uevent_from_sysfs(const char *sysroot, const char *dirpath, struct uevent *ev) {
    char upath[1024];
    if ((size_t)snprintf(upath, sizeof upath, "%s/uevent", dirpath) >= sizeof upath) return -1;
    FILE *f = fopen(upath, "r");
    if (!f) return -1;

    memset(ev, 0, sizeof *ev);

    /* Set ACTION=add */
    safe_copy(ev->key[ev->n], "ACTION", UE_KEY_MAX);
    safe_copy(ev->val[ev->n], "add", UE_VAL_MAX);
    ev->n++;

    /* Set DEVPATH (strip sysroot prefix) */
    const char *devpath = dirpath;
    size_t sroot_len = strlen(sysroot);
    if (strncmp(dirpath, sysroot, sroot_len) == 0) devpath = dirpath + sroot_len;
    safe_copy(ev->key[ev->n], "DEVPATH", UE_KEY_MAX);
    safe_copy(ev->val[ev->n], devpath, UE_VAL_MAX);
    ev->n++;

    /* Resolve SUBSYSTEM from subsystem symlink */
    char sublink[1024], subtarget[1024];
    if ((size_t)snprintf(sublink, sizeof sublink, "%s/subsystem", dirpath) < sizeof sublink) {
        ssize_t slen = readlink(sublink, subtarget, sizeof(subtarget) - 1);
        if (slen > 0) {
            subtarget[slen] = '\0';
            char *bname = strrchr(subtarget, '/');
            const char *subsys = bname ? bname + 1 : subtarget;
            safe_copy(ev->key[ev->n], "SUBSYSTEM", UE_KEY_MAX);
            safe_copy(ev->val[ev->n], subsys, UE_VAL_MAX);
            ev->n++;
        }
    }

    /* Read KEY=VALUE lines from uevent file */
    char line[512];
    while (fgets(line, sizeof line, f) && ev->n < UE_MAX_KEYS) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';
        
        /* Skip duplicate ACTION, DEVPATH, SUBSYSTEM if in file */
        if (uevent_get(ev, line) != NULL) continue;

        safe_copy(ev->key[ev->n], line, UE_KEY_MAX);
        safe_copy(ev->val[ev->n], val, UE_VAL_MAX);
        ev->n++;
    }

    fclose(f);
    return 0;
}

#include <ftw.h>

static char  **g_coldplug_paths = NULL;   /* collected device dir paths */
static size_t  g_coldplug_np = 0, g_coldplug_cap = 0;

/* Collect-only: gather each device's dir path (strip "/uevent"); dispatch is
 * deferred until after a sort, so parents precede children. */
static int coldplug_collect_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag != FTW_F) return 0;
    const char *bname = strrchr(fpath, '/');
    if (!bname || strcmp(bname + 1, "uevent") != 0) return 0;
    size_t dlen = (size_t)(bname - fpath);   /* dir path (without "/uevent") */
    if (g_coldplug_np == g_coldplug_cap) {
        size_t ncap = g_coldplug_cap ? g_coldplug_cap * 2 : 256;
        char **np = realloc(g_coldplug_paths, ncap * sizeof *np);
        if (!np) return 1;                   /* OOM -> stop walk, dispatch what we have */
        g_coldplug_paths = np; g_coldplug_cap = ncap;
    }
    char *d = malloc(dlen + 1);
    if (!d) return 1;
    memcpy(d, fpath, dlen); d[dlen] = '\0';
    g_coldplug_paths[g_coldplug_np++] = d;
    return 0;
}

static int coldplug_path_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static inline int coldplug_walk_root(const char *sysroot, void (*on_event)(struct uevent *ev)) {
    char devroot[1024];
    if ((size_t)snprintf(devroot, sizeof devroot, "%s/devices", sysroot) >= sizeof devroot) return -1;
    struct stat st;
    if (stat(devroot, &st) != 0) return 0;  /* missing sysfs dir -> no-op */

    g_coldplug_paths = NULL; g_coldplug_np = 0; g_coldplug_cap = 0;
    int rc = nftw(devroot, coldplug_collect_cb, 32, FTW_PHYS);
    /* A parent's devpath is a prefix of its children's, so a lexicographic sort
     * dispatches every parent before its children — required so a child's
     * IMPORT{parent} finds the parent's DB record already written. */
    qsort(g_coldplug_paths, g_coldplug_np, sizeof *g_coldplug_paths, coldplug_path_cmp);
    for (size_t i = 0; i < g_coldplug_np; i++) {
        struct uevent ev;
        if (uevent_from_sysfs(sysroot, g_coldplug_paths[i], &ev) == 0 && on_event)
            on_event(&ev);
        free(g_coldplug_paths[i]);
    }
    free(g_coldplug_paths);
    g_coldplug_paths = NULL; g_coldplug_np = g_coldplug_cap = 0;
    return rc;
}

#define UDEV_MONITOR_MAGIC 0xfeedcafeu

static inline uint32_t murmur2(const char *str) {
    const uint32_t m = 0x5bd1e995u;
    const int r = 24;
    size_t len = strlen(str);
    const unsigned char *data = (const unsigned char *)str;
    uint32_t h = (uint32_t)len;   /* seed 0 */
    while (len >= 4) {
        uint32_t k = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                     ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        k *= m; k ^= k >> r; k *= m;
        h *= m; h ^= k;
        data += 4; len -= 4;
    }
    switch (len) {
        case 3: h ^= (uint32_t)data[2] << 16; /* fall through */
        case 2: h ^= (uint32_t)data[1] << 8;  /* fall through */
        case 1: h ^= (uint32_t)data[0];
                h *= m;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}

static inline ssize_t libudev_frame_build(const struct uevent *ev, char *buf, size_t bufsz) {
    const char *sub = uevent_get(ev, "SUBSYSTEM");
    if (!uevent_get(ev, "ACTION") || !uevent_get(ev, "DEVPATH") || !sub) return -1;
    if (bufsz < 40) return -1;

    size_t plen = 0;
    for (int i = 0; i < ev->n; i++) {
        size_t klen = strlen(ev->key[i]);
        size_t vlen = strlen(ev->val[i]);
        size_t rec = klen + 1 + vlen + 1;   /* KEY=VALUE\0 */
        if (40 + plen + rec > bufsz) return -1;
        char *p = buf + 40 + plen;
        memcpy(p, ev->key[i], klen); p += klen;
        *p++ = '=';
        memcpy(p, ev->val[i], vlen); p += vlen;
        *p = '\0';
        plen += rec;
    }

    const char *devtype = uevent_get(ev, "DEVTYPE");
    memset(buf, 0, 40);
    memcpy(buf, "libudev", 7);   /* buf[7] left NUL by memset */
    uint32_t magic  = htobe32(UDEV_MONITOR_MAGIC);
    uint32_t hdrsz  = 40, poff = 40, plen32 = (uint32_t)plen;
    uint32_t subh   = htobe32(murmur2(sub));
    uint32_t dth    = devtype ? htobe32(murmur2(devtype)) : 0;
    memcpy(buf + 8,  &magic,  4);
    memcpy(buf + 12, &hdrsz,  4);
    memcpy(buf + 16, &poff,   4);
    memcpy(buf + 20, &plen32, 4);
    memcpy(buf + 24, &subh,   4);
    memcpy(buf + 28, &dth,    4);
    /* buf+32, buf+36 (bloom hi/lo) already zero */
    return (ssize_t)(40 + plen);
}

#endif /* SCHEMA_UDEV_H */

