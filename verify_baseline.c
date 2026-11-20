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

/* the state dir, made and locked down; the fd, or -1 */
static int state_dir_open(const char *dir) {
    struct statfs sf;
    struct stat st;
    int dfd, e;

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
    return dfd;
fail:
    e = errno;
    close(dfd);
    errno = e;
    return -1;
}

/* NAME in DFD replaced by the two parts, via NAME.tmp + rename, 0600 */
static int put_atomic(int dfd, const char *name, const char *a, size_t an, const char *b, size_t bn) {
    char tmp[64];
    struct stat st;
    int fd, e;

    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode)) {
        errno = ELOOP;
        return -1;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", name);
    unlinkat(dfd, tmp, 0);
    fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (fchmod(fd, 0600) < 0 || write_all(fd, a, an) < 0 || write_all(fd, b, bn) < 0 || fsync(fd) < 0) {
        e = errno;
        close(fd);
        unlinkat(dfd, tmp, 0);
        errno = e;
        return -1;
    }
    close(fd);
    if (renameat(dfd, tmp, dfd, name) < 0) {
        e = errno;
        unlinkat(dfd, tmp, 0);
        errno = e;
        return -1;
    }
    return 0;
}

int verify_state_put(const char *dir, const char *name, const char *data, size_t len) {
    int dfd = state_dir_open(dir), r, e;
    if (dfd < 0) return -1;
    r = put_atomic(dfd, name, data, len, "", 0);
    e = errno;
    if (r == 0) fsync(dfd);
    close(dfd);
    errno = e;
    return r;
}

int baseline_write(const char *dir, const char *check_out, size_t len, time_t now) {
    const char *l2;
    char *head;
    int dfd, fd, r, e;

    /* an old binary without --check prints usage, not a baseline */
    if (len < sizeof BASELINE_MAGIC - 1 || memcmp(check_out, BASELINE_MAGIC, sizeof BASELINE_MAGIC - 1) ||
        !(l2 = memchr(check_out + sizeof BASELINE_MAGIC - 1, '\n', len - (sizeof BASELINE_MAGIC - 1)))) {
        errno = EINVAL;
        return -1;
    }
    l2++;
    if ((dfd = state_dir_open(dir)) < 0) return -1;
    /* the first two lines, then created=, then the rest */
    if (asprintf(&head, "%.*screated=%lld\n", (int)(l2 - check_out), check_out, (long long)now) < 0) {
        close(dfd);
        errno = ENOMEM;
        return -1;
    }
    r = put_atomic(dfd, "baseline", head, strlen(head), l2, len - (size_t)(l2 - check_out));
    free(head);
    if (r == 0) {
        fd = openat(dfd, "established", O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) r = -1;
        else close(fd);
    }
    e = errno;
    if (r == 0) fsync(dfd);
    close(dfd);
    errno = e;
    return r;
}

/* the value of service.<name>.<key>= at a line start, or NULL */
static const char *kv_field(const char *kv, const char *name, size_t nlen, const char *key) {
    char needle[512];
    int n = snprintf(needle, sizeof needle, "service.%.*s.%s=", (int)nlen, name, key);
    for (const char *p = kv; (p = strstr(p, needle)); p += n)
        if (p == kv || p[-1] == '\n') return p + n;
    return NULL;
}

static int val_is(const char *v, const char *want) {
    size_t n = strlen(want);
    return v && !strncmp(v, want, n) && (v[n] == '\n' || !v[n]);
}

/* engine.c svc_failed() decides, sent as failed=; an older PID 1 without it
 * gets the same split by reason: EXCISED with none (a stop, standby) or bind
 * (waiting for its bound) is not a failure. A NEW_PROCESS held on its
 * wait_socket (waiting=) is idle until a connection, not starting. */
static int state_ok(const char *kv, const char *name, size_t nlen, const char *val, size_t vlen) {
    static const char *const good[] = { "FUNDAMENTAL", "SETTLED", "FULL_TRUST", "PERFECT" };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++)
        if (vlen == strlen(good[i]) && !memcmp(val, good[i], vlen)) return 1;
    int excised = vlen == 7 && !memcmp(val, "EXCISED", 7), dormant = vlen == 7 && !memcmp(val, "DORMANT", 7);
    const char *f = kv_field(kv, name, nlen, "failed");
    if (f && (excised || dormant)) return val_is(f, "0");
    if (excised) {
        const char *r = kv_field(kv, name, nlen, "excise_reason");
        return val_is(r, "none") || val_is(r, "bind");
    }
    return vlen == 11 && !memcmp(val, "NEW_PROCESS", 11) && kv_field(kv, name, nlen, "waiting");
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
                if (!state_ok(status_kv, name, nlen, val, vlen)) healthy = 0;
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

/* 15 min, or the window plus 5 min: a long window must still get a chance */
uint64_t settle_give_up_ms(unsigned window_s) {
    uint64_t s = (uint64_t)window_s + 5 * 60;
    return (s > 15 * 60 ? s : 15 * 60) * 1000;
}

/* By the exe link, not comm: an init=/sbin/init symlink install runs as
 * "init". A binary replaced on disk reads "<path> (deleted)". */
int pid1_exe_is_schema_init(const char *exe) {
    const char *b = strrchr(exe, '/');
    b = b ? b + 1 : exe;
    return strncmp(b, "schema-init", 11) == 0;
}

/* the value of a kv line key=…, NULL when there is none; its length in *len */
static const char *kv_line(const char *kv, const char *key, size_t *len) {
    size_t kl = strlen(key);
    for (const char *b = kv; b; b = (b = strchr(b, '\n')) ? b + 1 : NULL)
        if (!strncmp(b, key, kl)) {
            *len = strcspn(b + kl, "\n");
            return b + kl;
        }
    return NULL;
}

int reexecd_since_boot(const char *status_kv, const char *version_out, const char *image) {
    size_t bl, il;
    const char *b = kv_line(status_kv, "booted_version=", &bl), *i = kv_line(status_kv, "booted_image=", &il);
    if (i && (strlen(image) != il || memcmp(i, image, il))) return 1;
    if (!b) return 0;
    if (strncmp(version_out, "schema-init ", 12)) return 1;
    const char *v = version_out + 12;
    size_t vl = strcspn(v, "\n");
    return bl != vl || memcmp(b, v, bl);
}
