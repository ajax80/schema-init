#ifndef SCHEMA_UDEV_H
#define SCHEMA_UDEV_H

#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>

#define UE_MAX_KEYS 32
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

#define SCHEMA_DEV_DIR "/dev/schema"

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

#endif /* SCHEMA_UDEV_H */
