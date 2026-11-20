#include "verify_baseline.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int baseline_write(const char *dir, const char *check_out, size_t len, time_t now) {
    const char *l2;
    char created[32];
    struct statfs sf;
    struct stat st;
    int dfd, fd, e;

    /* an old binary without --check prints usage, not a baseline */
    if (len < sizeof BASELINE_MAGIC - 1 || memcmp(check_out, BASELINE_MAGIC, sizeof BASELINE_MAGIC - 1) ||
        !(l2 = memchr(check_out + sizeof BASELINE_MAGIC - 1, '\n', len - (sizeof BASELINE_MAGIC - 1)))) {
        errno = EINVAL;
        return -1;
    }
    l2++;
    if (mkdir(dir, 0700) < 0 && errno != EEXIST) return -1;
    /* O_NOFOLLOW|O_DIRECTORY reports ENOTDIR for a symlink, so ask directly */
    if (lstat(dir, &st) == 0 && S_ISLNK(st.st_mode)) {
        errno = ELOOP;
        return -1;
    }
    dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return -1;
    /* a dir someone else owns could be swapped under us; one we own is ours to lock down */
    if (fstat(dfd, &st) < 0 || st.st_uid != geteuid()) {
        close(dfd);
        errno = EPERM;
        return -1;
    }
    if (fchmod(dfd, 0700) < 0) goto fail;
    /* /var not mounted yet: a write would land in the tmpfs underneath and vanish */
    if (fstatfs(dfd, &sf) < 0 || sf.f_type == TMPFS_MAGIC) {
        close(dfd);
        errno = EROFS;
        return -1;
    }
    if (fstatat(dfd, "baseline", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode)) {
        close(dfd);
        errno = ELOOP;
        return -1;
    }
    snprintf(created, sizeof created, "created=%lld\n", (long long)now);
    unlinkat(dfd, "baseline.tmp", 0);
    fd = openat(dfd, "baseline.tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) goto fail;
    if (fchmod(fd, 0600) < 0 ||
        write_all(fd, check_out, (size_t)(l2 - check_out)) < 0 ||
        write_all(fd, created, strlen(created)) < 0 ||
        write_all(fd, l2, len - (size_t)(l2 - check_out)) < 0 ||
        fsync(fd) < 0) {
        e = errno;
        close(fd);
        unlinkat(dfd, "baseline.tmp", 0);
        errno = e;
        goto fail;
    }
    close(fd);
    if (renameat(dfd, "baseline.tmp", dfd, "baseline") < 0) {
        e = errno;
        unlinkat(dfd, "baseline.tmp", 0);
        errno = e;
        goto fail;
    }
    fd = openat(dfd, "established", O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) goto fail;
    close(fd);
    fsync(dfd);
    close(dfd);
    return 0;
fail:
    e = errno;
    close(dfd);
    errno = e;
    return -1;
}

/* EXCISED with reason none (a stop, standby) or bind (waiting for its bound)
 * is not a failure — the same split as svc_failed() in engine.c. */
static int excised_ok(const char *kv, const char *name, size_t nlen) {
    static const char *const ok[] = { "none", "bind" };
    char needle[512];
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        const char *p = kv;
        int n = snprintf(needle, sizeof needle, "service.%.*s.excise_reason=%s\n", (int)nlen, name, ok[i]);
        while ((p = strstr(p, needle))) {
            if (p == kv || p[-1] == '\n') return 1;
            p += n;
        }
    }
    return 0;
}

int settle_step(settle_t *st, const char *status_kv, uint64_t now_ms, unsigned window_s) {
    size_t cap = 4096, used = 0;
    char *set = calloc(1, cap), sig[65];
    int healthy = 1;
    const char *line = status_kv;

    if (!set) return 0;
    while (*line) {
        const char *nl = strchr(line, '\n'), *end = nl ? nl : line + strlen(line);
        const char *eq = memchr(line, '=', (size_t)(end - line)), *dot = NULL;
        if (eq && !strncmp(line, "service.", 8)) {
            for (const char *q = line + 8; q < eq; q++) if (*q == '.') dot = q;
        }
        if (dot) {
            const char *name = line + 8, *key = dot + 1, *val = eq + 1;
            size_t nlen = (size_t)(dot - name), klen = (size_t)(eq - key), vlen = (size_t)(end - val);
            if (klen == 5 && !memcmp(key, "state", 5)) {
                static const char *const good[] = { "FUNDAMENTAL", "SETTLED", "FULL_TRUST", "PERFECT" };
                int g = 0;
                for (size_t i = 0; i < sizeof good / sizeof good[0]; i++)
                    if (vlen == strlen(good[i]) && !memcmp(val, good[i], vlen)) g = 1;
                if (!g && !(vlen == 7 && !memcmp(val, "EXCISED", 7) && excised_ok(status_kv, name, nlen)))
                    healthy = 0;
            } else if (klen == 8 && !memcmp(key, "restarts", 8)) {
                size_t need = nlen + vlen + 2;
                if (used + need > cap) {
                    char *nb;
                    while (used + need > cap) cap *= 2;
                    if (!(nb = realloc(set, cap))) { free(set); return 0; }
                    set = nb;
                }
                memcpy(set + used, name, nlen);
                set[used + nlen] = '=';
                memcpy(set + used + nlen + 1, val, vlen);
                set[used + need - 1] = '\n';
                used += need;
            }
        }
        line = nl ? nl + 1 : end;
    }
    sha256_hex(set, used, sig);
    free(set);
    if (!healthy || strcmp(sig, st->sig)) {
        st->since_ms = now_ms;
        memcpy(st->sig, sig, sizeof sig);
        return 0;
    }
    return now_ms - st->since_ms >= (uint64_t)window_s * 1000;
}
