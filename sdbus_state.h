#ifndef SDBUS_STATE_H
#define SDBUS_STATE_H

/* Broker state snapshot for a planned restart. The old broker serialises its
   connections, name registry and pending replies into a memfd; the new one reads
   it back, pairing each conn with the fd PID 1 handed over (same order). Fds and
   pointers are never stored, only their counts and the ids that tie things
   together. Native-endian fixed-width integers; strings and blobs are u32 len +
   bytes. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "sdbus_conn.h"
#include "sdbus_names.h"
#include "sdbus_reply.h"

#define SDBUS_STATE_MAGIC 0x53444253u
#define SDBUS_STATE_VERSION 1u

typedef struct { unsigned next_id; unsigned next_unique; uint32_t driver_serial; } sdbus_state_hdr;

typedef struct { unsigned char *b; size_t len, cap; int err; } sdbus__sw;

static inline void sdbus__sw_put(sdbus__sw *w, const void *p, size_t n) {
    if (w->err) return;
    if (w->len + n > w->cap) {
        size_t nc = (w->len + n) * 2 + 256;
        unsigned char *nb = realloc(w->b, nc);
        if (!nb) { w->err = 1; return; }
        w->b = nb; w->cap = nc;
    }
    if (n) memcpy(w->b + w->len, p, n);
    w->len += n;
}

static inline void sdbus__sw_u32(sdbus__sw *w, uint32_t v) { sdbus__sw_put(w, &v, sizeof v); }
static inline void sdbus__sw_i32(sdbus__sw *w, int32_t v) { sdbus__sw_put(w, &v, sizeof v); }
static inline void sdbus__sw_blob(sdbus__sw *w, const void *p, size_t n) {
    sdbus__sw_u32(w, (uint32_t)n);
    sdbus__sw_put(w, p, n);
}
static inline void sdbus__sw_str(sdbus__sw *w, const char *s) { sdbus__sw_blob(w, s, s ? strlen(s) : 0); }

static inline void sdbus__sw_matches(sdbus__sw *w, const sdbus_matchset *m) {
    sdbus__sw_u32(w, m ? (uint32_t)m->n : 0);
    for (int i = 0; m && i < m->n; i++) sdbus__sw_str(w, m->rules[i].raw);
}

static inline int sdbus_state_write(int memfd, const sdbus_state_hdr *h, sdbus_conn *const *conns,
                                    int nconns, const sdbus_names *names, const sdbus_replies *replies) {
    sdbus__sw w = {0};
    sdbus__sw_u32(&w, SDBUS_STATE_MAGIC);
    sdbus__sw_u32(&w, SDBUS_STATE_VERSION);
    sdbus__sw_u32(&w, h->next_id);
    sdbus__sw_u32(&w, h->next_unique);
    sdbus__sw_u32(&w, h->driver_serial);

    sdbus__sw_u32(&w, (uint32_t)nconns);
    for (int i = 0; i < nconns; i++) {
        const sdbus_conn *c = conns[i];
        sdbus__sw_i32(&w, c->id);
        sdbus__sw_u32(&w, (uint32_t)(c->authed ? 1 : 0) | (c->auth_ok ? 2 : 0) | (c->said_hello ? 4 : 0)
                          | (c->negotiated_fd ? 8 : 0) | (c->saw_nul ? 16 : 0));
        sdbus__sw_u32(&w, (uint32_t)c->uid);
        sdbus__sw_u32(&w, (uint32_t)c->gid);
        sdbus__sw_i32(&w, (int32_t)c->pid);
        sdbus__sw_u32(&w, (uint32_t)c->n_gids);
        for (int g = 0; g < c->n_gids; g++) sdbus__sw_i32(&w, c->gids[g]);
        sdbus__sw_str(&w, c->unique);
        sdbus__sw_blob(&w, c->in, (size_t)c->in_len);
        sdbus__sw_u32(&w, (uint32_t)c->n_pending_fds);
        sdbus__sw_u32(&w, c->is_monitor ? 1 : 0);
        sdbus__sw_matches(&w, c->matches);
        sdbus__sw_matches(&w, c->mon_matches);
    }

    sdbus__sw_u32(&w, (uint32_t)names->n_names);
    for (int i = 0; i < names->n_names; i++) {
        const sdbus_name_ent *e = &names->names[i];
        sdbus__sw_str(&w, e->name);
        sdbus__sw_u32(&w, (uint32_t)e->n_holders);
        for (int k = 0; k < e->n_holders; k++) {
            sdbus__sw_i32(&w, e->holders[k].conn_id);
            sdbus__sw_u32(&w, e->holders[k].flags);
        }
    }

    long now = sdbus__now_ms();
    uint32_t nlive = 0;
    for (int i = 0; i < replies->n; i++) if (replies->ents[i].live) nlive++;
    sdbus__sw_u32(&w, nlive);
    for (int i = 0; i < replies->n; i++) {
        const sdbus_reply_ent *r = &replies->ents[i];
        if (!r->live) continue;
        long left = r->deadline - now;
        sdbus__sw_i32(&w, r->callee);
        sdbus__sw_i32(&w, r->caller);
        sdbus__sw_u32(&w, r->serial);
        sdbus__sw_u32(&w, left > 0 ? (uint32_t)left : 0);
    }

    int rc = -1;
    if (!w.err && lseek(memfd, 0, SEEK_SET) == 0 && ftruncate(memfd, 0) == 0) {
        size_t off = 0;
        rc = 0;
        while (off < w.len) {
            ssize_t n = write(memfd, w.b + off, w.len - off);
            if (n <= 0) { rc = -1; break; }
            off += (size_t)n;
        }
    }
    free(w.b);
    return rc;
}

typedef struct { const unsigned char *p; size_t len, off; int bad; } sdbus__sr;

static inline int sdbus__sr_get(sdbus__sr *r, void *out, size_t n) {
    if (r->bad || n > r->len - r->off) { r->bad = 1; return 0; }
    if (n) memcpy(out, r->p + r->off, n);
    r->off += n;
    return 1;
}

static inline uint32_t sdbus__sr_u32(sdbus__sr *r) { uint32_t v = 0; sdbus__sr_get(r, &v, sizeof v); return v; }
static inline int32_t sdbus__sr_i32(sdbus__sr *r) { int32_t v = 0; sdbus__sr_get(r, &v, sizeof v); return v; }

/* malloc'd NUL-terminated copy of the next string, or NULL (check r->bad) */
static inline char *sdbus__sr_str(sdbus__sr *r) {
    uint32_t n = sdbus__sr_u32(r);
    if (r->bad || n > r->len - r->off) { r->bad = 1; return NULL; }
    char *s = malloc((size_t)n + 1);
    if (!s) { r->bad = 1; return NULL; }
    memcpy(s, r->p + r->off, n);
    s[n] = '\0';
    r->off += n;
    return s;
}

static inline sdbus_matchset *sdbus__sr_matches(sdbus__sr *r) {
    uint32_t n = sdbus__sr_u32(r);
    if (r->bad || n > r->len - r->off) { r->bad = 1; return NULL; }
    if (n == 0) return NULL;
    sdbus_matchset *m = sdbus_match_new();
    for (uint32_t i = 0; i < n; i++) {
        char *raw = sdbus__sr_str(r);
        if (!raw || sdbus_match_add(m, raw) != 0) { free(raw); r->bad = 1; break; }
        free(raw);
    }
    if (r->bad) { sdbus_match_free(m); return NULL; }
    return m;
}

static inline void sdbus__state_drop(sdbus_conn **conns, int n) {
    for (int i = 0; i < n; i++) {
        if (!conns[i]) continue;
        sdbus_conn_free_fields(conns[i]);
        free(conns[i]);
        conns[i] = NULL;
    }
}

static inline int sdbus_state_read(int memfd, sdbus_state_hdr *h, sdbus_conn **conns, int maxconns,
                                   int *nconns, const int *conn_fds, int nconn_fds,
                                   const int *pfds, int npfds, sdbus_names *names,
                                   sdbus_replies *replies) {
    *nconns = 0;
    struct stat st;
    if (fstat(memfd, &st) != 0 || st.st_size < 0) return -1;
    size_t sz = (size_t)st.st_size;
    unsigned char *buf = malloc(sz ? sz : 1);
    if (!buf) return -1;
    size_t got = 0;
    while (got < sz) {
        ssize_t n = pread(memfd, buf + got, sz - got, (off_t)got);
        if (n <= 0) { free(buf); return -1; }
        got += (size_t)n;
    }

    sdbus__sr r = { buf, sz, 0, 0 };
    int rc = -1, built = 0, used_pfds = 0;
    uint32_t nc = 0;
    if (sdbus__sr_u32(&r) != SDBUS_STATE_MAGIC) goto out;
    if (sdbus__sr_u32(&r) != SDBUS_STATE_VERSION) { rc = r.bad ? -1 : -2; goto out; }
    h->next_id = sdbus__sr_u32(&r);
    h->next_unique = sdbus__sr_u32(&r);
    h->driver_serial = sdbus__sr_u32(&r);
    nc = sdbus__sr_u32(&r);
    if (r.bad || nc > (uint32_t)maxconns || (int)nc != nconn_fds) goto out;

    for (uint32_t i = 0; i < nc; i++) {
        sdbus_conn *c = calloc(1, sizeof *c);
        if (!c) goto out;
        conns[built++] = c;
        c->fd = conn_fds[i];
        c->id = sdbus__sr_i32(&r);
        uint32_t fl = sdbus__sr_u32(&r);
        c->authed = !!(fl & 1); c->auth_ok = !!(fl & 2); c->said_hello = !!(fl & 4);
        c->negotiated_fd = !!(fl & 8); c->saw_nul = !!(fl & 16);
        c->uid = (uid_t)sdbus__sr_u32(&r);
        c->gid = (gid_t)sdbus__sr_u32(&r);
        c->pid = (pid_t)sdbus__sr_i32(&r);
        uint32_t ng = sdbus__sr_u32(&r);
        if (r.bad || ng > SDBUS_MAX_GIDS) goto out;
        for (uint32_t g = 0; g < ng; g++) c->gids[g] = sdbus__sr_i32(&r);
        c->n_gids = (int)ng;
        char *uq = sdbus__sr_str(&r);
        if (!uq) goto out;
        if (*uq) {
            names->uniques = realloc(names->uniques, (names->n_uniques + 1) * sizeof *names->uniques);
            names->uniques[names->n_uniques].conn_id = c->id;
            names->uniques[names->n_uniques].unique = uq;
            c->unique = names->uniques[names->n_uniques++].unique;
        } else {
            free(uq);
        }
        uint32_t inl = sdbus__sr_u32(&r);
        if (r.bad || inl > r.len - r.off) goto out;
        if (inl) { sdbus_conn_in_append(c, r.p + r.off, (int)inl); r.off += inl; }
        uint32_t np = sdbus__sr_u32(&r);
        if (r.bad || np > SDBUS_MAX_PENDING_FDS || (int)np > npfds - used_pfds) goto out;
        for (uint32_t k = 0; k < np; k++) c->pending_fds[k] = pfds[used_pfds++];
        c->n_pending_fds = (int)np;
        c->is_monitor = (int)sdbus__sr_u32(&r) & 1;
        c->matches = sdbus__sr_matches(&r);
        c->mon_matches = sdbus__sr_matches(&r);
        if (r.bad) goto out;
    }

    uint32_t nn = sdbus__sr_u32(&r);
    if (r.bad || nn > r.len - r.off) goto out;
    for (uint32_t i = 0; i < nn; i++) {
        char *nm = sdbus__sr_str(&r);
        if (!nm) goto out;
        sdbus_name_ent *e = sdbus__name_intern(names, nm);
        free(nm);
        uint32_t nh = sdbus__sr_u32(&r);
        if (r.bad || nh > r.len - r.off) goto out;
        for (uint32_t k = 0; k < nh; k++) {
            int cid = sdbus__sr_i32(&r);
            unsigned f = sdbus__sr_u32(&r);
            if (r.bad) goto out;
            sdbus__holder_insert(e, e->n_holders, cid, f);
        }
    }

    uint32_t nr = sdbus__sr_u32(&r);
    if (r.bad || nr > r.len - r.off) goto out;
    long now = sdbus__now_ms();
    for (uint32_t i = 0; i < nr; i++) {
        sdbus_reply_ent e;
        e.callee = sdbus__sr_i32(&r);
        e.caller = sdbus__sr_i32(&r);
        e.serial = sdbus__sr_u32(&r);
        e.deadline = now + (long)sdbus__sr_u32(&r);
        e.live = 1;
        if (r.bad) goto out;
        replies->ents = realloc(replies->ents, (replies->n + 1) * sizeof *replies->ents);
        replies->ents[replies->n++] = e;
    }

    names->next_unique = h->next_unique;
    *nconns = (int)nc;
    rc = 0;
out:
    if (rc != 0) sdbus__state_drop(conns, built);
    free(buf);
    return rc;
}

#endif
