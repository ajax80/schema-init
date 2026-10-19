#ifndef FDSTORE_H
#define FDSTORE_H

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FDSTORE_MAX_TOTAL 4096

typedef struct { char svc[64]; char name[64]; int fd; } fdstore_ent;

static fdstore_ent *fdstore_v;
static int fdstore_n, fdstore_cap;

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

static inline int fdstore_add(const char *svc, int fd, const char *name, int max) {
    if (!name || !*name) name = "stored";
    if (max <= 0 || strlen(svc) >= sizeof fdstore_v->svc ||
        !fdstore_valid_name(name) || fdstore_n >= FDSTORE_MAX_TOTAL ||
        fdstore_count(svc) >= max) {
        close(fd);
        return -1;
    }
    if (fdstore_n == fdstore_cap) {
        int nc = fdstore_cap ? fdstore_cap * 2 : 16;
        fdstore_ent *nv = realloc(fdstore_v, (size_t)nc * sizeof *nv);
        if (!nv) { close(fd); return -1; }
        fdstore_v = nv;
        fdstore_cap = nc;
    }
    fdstore_ent *e = &fdstore_v[fdstore_n++];
    strcpy(e->svc, svc);
    strcpy(e->name, name);
    e->fd = fd;
    return 0;
}

static inline int fdstore_remove(const char *svc, const char *name) {
    int removed = 0, w = 0;
    for (int i = 0; i < fdstore_n; i++) {
        if (!strcmp(fdstore_v[i].svc, svc) && !strcmp(fdstore_v[i].name, name)) {
            close(fdstore_v[i].fd);
            removed++;
        } else {
            fdstore_v[w++] = fdstore_v[i];
        }
    }
    fdstore_n = w;
    return removed;
}

static inline int fdstore_drop(const char *svc) {
    int removed = 0, w = 0;
    for (int i = 0; i < fdstore_n; i++) {
        if (!strcmp(fdstore_v[i].svc, svc)) {
            close(fdstore_v[i].fd);
            removed++;
        } else {
            fdstore_v[w++] = fdstore_v[i];
        }
    }
    fdstore_n = w;
    return removed;
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
        char svc[64];
        strcpy(svc, fdstore_v[i].svc);
        if (keep(svc, ctx)) { i++; continue; }
        closed += fdstore_drop(svc);
    }
    return closed;
}

#endif
