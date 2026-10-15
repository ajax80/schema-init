#ifndef SVC_DROPINS_H
#define SVC_DROPINS_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SVC_DIR         "/etc/schema-init/services"
#define SVC_DROPIN_MAX  32
#define SVC_DROPIN_PATH 512

/* Locale-free: PID 1 and schema-ctl cat must agree on the order. */
static inline int svc_dropin_cmp(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

static inline int svc_read_line(FILE *f, char *buf, size_t len) {
    int c;
    for (;;) {
        if (!fgets(buf, len, f)) return 0;
        if (strchr(buf, '\n') || (c = getc(f)) == '\n' || c == EOF) return 1;
        while ((c = getc(f)) != '\n' && c != EOF)
            ;
        char c0 = buf[strspn(buf, " \t")];
        if (c0 != '#' && c0 != ';') return -1;
    }
}

static inline int svc_dir_trusted(const struct stat *st) {
    return (st->st_uid == 0 || st->st_uid == geteuid()) &&
           (!(st->st_mode & 022) || (st->st_mode & S_ISVTX));
}

static inline int svc_dropin_filter(const struct dirent *e) {
    size_t n = strlen(e->d_name);
    return e->d_name[0] != '.' && n > 5 && strcmp(e->d_name + n - 5, ".conf") == 0;
}

static inline int svc_dropin_scan(const char *dir, char (*out)[SVC_DROPIN_PATH], int count, int max) {
    struct dirent **ents;
    struct stat ds;
    if (stat(dir, &ds) == 0 && S_ISDIR(ds.st_mode) && !svc_dir_trusted(&ds)) return -1;
    int n = scandir(dir, &ents, svc_dropin_filter, svc_dropin_cmp);
    if (n < 0) return count;
    for (int i = 0; i < n; i++) {
        char p[SVC_DROPIN_PATH];
        struct stat st;
        if (snprintf(p, sizeof p, "%s/%s", dir, ents[i]->d_name) >= (int)sizeof p)
            fprintf(stderr, "[schema-init] WARN: drop-in path too long in %s — skipped\n", dir);
        else if (stat(p, &st) < 0 || !S_ISREG(st.st_mode))
            fprintf(stderr, "[schema-init] WARN: drop-in %s is not a regular file — skipped\n", p);
        else if (count >= max)
            fprintf(stderr, "[schema-init] WARN: more than %d drop-ins — %s skipped\n", max, p);
        else
            snprintf(out[count++], SVC_DROPIN_PATH, "%s", p);
        free(ents[i]);
    }
    free(ents);
    return count;
}

/* Drop-ins for the .svc at `path`, in apply order: foo@.svc.d/ (instances
 * only), then <path>.d/. Returns how many were written to `out`. */
static inline int svc_dropin_list(const char *path, char (*out)[SVC_DROPIN_PATH], int max) {
    char dir[SVC_DROPIN_PATH];
    int count = 0;
    const char *slash = strrchr(path, '/');
    const char *at = strchr(slash ? slash + 1 : path, '@');
    size_t plen = strlen(path);
    if (at && at[1] && strcmp(at + 1, ".svc") != 0 && plen > 4 &&
        strcmp(path + plen - 4, ".svc") == 0) {
        snprintf(dir, sizeof dir, "%.*s@.svc.d", (int)(at - path), path);
        count = svc_dropin_scan(dir, out, count, max);
        if (count < 0) return -1;
    }
    snprintf(dir, sizeof dir, "%s.d", path);
    return svc_dropin_scan(dir, out, count, max);
}

#endif
