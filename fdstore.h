#ifndef FDSTORE_H
#define FDSTORE_H

#include <fcntl.h>
#include <stdio.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fdclose.h"

#define FDSTORE_MAX_TOTAL 4096
#define FDSTORE_HEADROOM 256

typedef struct { char svc[64]; char name[64]; int fd; } fdstore_ent;

extern fdstore_ent *fdstore_v;
extern int fdstore_n, fdstore_cap;

static inline int fdstore_valid_name(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n < 1 || n > 63) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return 0;
    }
    return 1;
}

static inline int fdstore_count(const char *svc) {
    int c = 0;
    for (int i = 0; i < fdstore_n; i++)
        if (!strcmp(fdstore_v[i].svc, svc)) c++;
    return c;
}

/* Stored fds must never eat the descriptors PID 1 needs for its own work
 * (spawning, logging, the control socket): keep HEADROOM below the limit. */
static inline int fdstore_room(int fd) {
    struct rlimit rl;
    return getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY ||
           (rlim_t)fd + FDSTORE_HEADROOM < rl.rlim_cur;
}

static inline int fdstore_add(const char *svc, int fd, const char *name, int max) {
    if (!name || !*name) name = "stored";
    if (max <= 0 || !fdstore_room(fd) || strlen(svc) >= sizeof fdstore_v->svc ||
        !fdstore_valid_name(name) || fdstore_n >= FDSTORE_MAX_TOTAL ||
        fdstore_count(svc) >= max)
        return -1;
    if (fdstore_n == fdstore_cap) {
        int nc = fdstore_cap ? fdstore_cap * 2 : 16;
        fdstore_ent *nv = realloc(fdstore_v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        fdstore_v = nv;
        fdstore_cap = nc;
    }
    fdstore_ent *e = &fdstore_v[fdstore_n++];
    strcpy(e->svc, svc);
    strcpy(e->name, name);
    e->fd = fd;
    return 0;
}

static inline int fdstore_take(const char *svc, const char *name) {
    int removed = 0, w = 0;
    int *gone = malloc((size_t)(fdstore_n ? fdstore_n : 1) * sizeof *gone);
    for (int i = 0; i < fdstore_n; i++) {
        if (!strcmp(fdstore_v[i].svc, svc) && (!name || !strcmp(fdstore_v[i].name, name))) {
            /* a stored fd came from a service: it closes through a closer */
            if (gone) gone[removed] = fdstore_v[i].fd;
            else close(fdstore_v[i].fd);
            removed++;
        } else {
            fdstore_v[w++] = fdstore_v[i];
        }
    }
    fdstore_n = w;
    if (gone) fd_close_detached(gone, removed);
    free(gone);
    return removed;
}

static inline int fdstore_remove(const char *svc, const char *name) {
    return fdstore_take(svc, name);
}

static inline int fdstore_drop(const char *svc) {
    return fdstore_take(svc, NULL);
}

static inline int fdstore_collect(const char *svc, int *fds, const char **names, int max) {
    int n = 0;
    for (int i = 0; i < fdstore_n && n < max; i++) {
        if (strcmp(fdstore_v[i].svc, svc)) continue;
        fds[n] = fdstore_v[i].fd;
        names[n] = fdstore_v[i].name;
        n++;
    }
    return n;
}

static inline int fdstore_fdnames(const char *svc, int listen_open, char *out, size_t n) {
    size_t o = 0;
    int first = 1;
    for (int i = 0; i < listen_open + fdstore_n; i++) {
        const char *nm;
        char tmp[80];
        if (i < listen_open) {
            snprintf(tmp, sizeof tmp, "%s.socket", svc);
            nm = tmp;
        } else {
            const fdstore_ent *e = &fdstore_v[i - listen_open];
            if (strcmp(e->svc, svc)) continue;
            nm = e->name;
        }
        size_t l = strlen(nm);
        if (o + l + (first ? 0 : 1) + 1 > n) return -1;
        if (!first) out[o++] = ':';
        memcpy(out + o, nm, l);
        o += l;
        first = 0;
    }
    if (n < 1) return -1;
    out[o] = '\0';
    return (int)o;
}

/* In the child, before exec: put the n fds at 3..3+n-1 for LISTEN_FDS.
 * Each is first dup'd above 3+n so no dup2 into 3+i can clobber a source
 * still waiting its turn. That briefly needs ~2n fds, which can pass the soft
 * NOFILE limit, so the soft limit is raised to the hard one for the shuffle
 * and put back so the service execs with the limit it was given. */
static inline int fdstore_place_fds(const int *src, int n) {
    struct rlimit old, cur;
    int raised = 0, rc = -1, i, made = 0;
    int *tmp = malloc((size_t)(n ? n : 1) * sizeof *tmp);
    if (!tmp) return -1;
    if (getrlimit(RLIMIT_NOFILE, &old) != 0) { free(tmp); return -1; }
    cur = old;
    rlim_t need = (rlim_t)n * 2 + 3 + 16;
    if (old.rlim_max != RLIM_INFINITY && old.rlim_max < need) { free(tmp); return -1; }
    if (old.rlim_cur != old.rlim_max) {
        cur.rlim_cur = old.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &cur) != 0) { free(tmp); return -1; }
        raised = 1;
    }
    for (i = 0; i < n; i++) {
        if ((tmp[i] = fcntl(src[i], F_DUPFD, 3 + n)) < 0) goto out;
        made++;
    }
    for (i = 0; i < n; i++)
        if (dup2(tmp[i], 3 + i) < 0) goto out;
    rc = 0;
out:
    for (i = 0; i < made; i++) close(tmp[i]);
    if (raised) setrlimit(RLIMIT_NOFILE, &old);
    free(tmp);
    return rc;
}

static inline void fdstore_set_cloexec(int on) {
    for (int i = 0; i < fdstore_n; i++) {
        int fl = fcntl(fdstore_v[i].fd, F_GETFD);
        if (fl < 0) continue;
        fcntl(fdstore_v[i].fd, F_SETFD, on ? (fl | FD_CLOEXEC) : (fl & ~FD_CLOEXEC));
    }
}

typedef int (*fdstore_keep_fn)(const char *svc, void *ctx);

static inline int fdstore_sweep(fdstore_keep_fn keep, void *ctx) {
    int closed = 0;
    for (int i = 0; i < fdstore_n; ) {
        /* a copy: fdstore_drop compacts the array under fdstore_v[i] */
        char svc[64];
        strcpy(svc, fdstore_v[i].svc);
        if (keep(svc, ctx)) { i++; continue; }
        closed += fdstore_drop(svc);
    }
    return closed;
}

#endif
