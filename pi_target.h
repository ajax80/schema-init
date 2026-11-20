#ifndef PI_TARGET_H
#define PI_TARGET_H

/* The Pi wrapper's inputs, shared with schema-init --check so both read the
 * next boot's target the same way. */
#include "pi_bootguard.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Streamed, not mmap'd: the Pi may be short on memory this early. */
static inline int pi_files_differ(const char *a, const char *b) {
    struct stat sa, sb;
    int fa = open(a, O_RDONLY | O_CLOEXEC);
    if (fa < 0) return 0;
    int fb = open(b, O_RDONLY | O_CLOEXEC);
    if (fb < 0) { close(fa); return 1; }
    int diff = fstat(fa, &sa) != 0 || fstat(fb, &sb) != 0 || sa.st_size != sb.st_size;
    if (!diff) {
        char x[4096], y[4096];
        for (;;) {
            ssize_t nx = read(fa, x, sizeof x), ny = read(fb, y, sizeof y);
            if (nx != ny || (nx > 0 && memcmp(x, y, (size_t)nx) != 0)) { diff = 1; break; }
            if (nx <= 0) break;
        }
    }
    close(fa);
    close(fb);
    return diff;
}

/* 1 if a name in dir a is missing from b or differs; recurses into drop-in dirs (foo.svc.d/). */
static inline int pi_dir_one_way(const char *a, const char *b, int depth) {
    DIR *d = opendir(a);
    if (!d) return 0;
    struct dirent *e;
    int diff = 0;
    while (!diff && (e = readdir(d))) {
        const char *n = e->d_name;
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) continue;
        /* promote() points these two at the known-good scripts, so they always differ by design */
        if (depth == 0 && (strcmp(n, "pi-bootok.svc") == 0 || strcmp(n, "pi-deadline.svc") == 0)) continue;
        char pa[PATH_MAX], pb[PATH_MAX];
        struct stat sa, sb;
        snprintf(pa, sizeof pa, "%s/%s", a, n);
        snprintf(pb, sizeof pb, "%s/%s", b, n);
        if (stat(pa, &sa) != 0) continue;
        if (stat(pb, &sb) != 0 || S_ISDIR(sa.st_mode) != S_ISDIR(sb.st_mode)) diff = 1;
        else if (S_ISDIR(sa.st_mode)) diff = depth < 1 && pi_dir_one_way(pa, pb, depth + 1);
        else diff = pi_files_differ(pa, pb);
    }
    closedir(d);
    return diff;
}

static inline void pi_state_load(const char *root, struct pi_state *st) {
    char path[PATH_MAX], buf[256];
    ssize_t n = 0;
    snprintf(path, sizeof path, "%s/var/lib/schema-init/pi-boot", root);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        n = read(fd, buf, sizeof buf - 1);
        close(fd);
    }
    buf[n > 0 ? n : 0] = 0;
    pi_state_parse(buf, st);
}

/* known-good is the whole set: a services-only edit that fails boot must still fall back to it */
static inline int pi_known_good_differs(const char *root) {
    char kg[PATH_MAX], cur[PATH_MAX], ksv[PATH_MAX], sv[PATH_MAX];
    struct stat ks;
    snprintf(kg, sizeof kg, "%s/var/lib/schema-init/known-good/schema-init", root);
    if (stat(kg, &ks) != 0) return 0;
    snprintf(cur, sizeof cur, "%s/usr/bin/schema-init", root);
    snprintf(ksv, sizeof ksv, "%s/var/lib/schema-init/known-good/services", root);
    snprintf(sv, sizeof sv, "%s/etc/schema-init/services", root);
    return pi_files_differ(kg, cur) || pi_dir_one_way(ksv, sv, 0) || pi_dir_one_way(sv, ksv, 0);
}

#endif
