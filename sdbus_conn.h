#ifndef SDBUS_CONN_H
#define SDBUS_CONN_H

/* Per-connection state and credential capture. The broker owns connection
   lifecycle; libdbus never sees the socket. Credentials come from SO_PEERCRED
   (authoritative uid/gid/pid) plus getgrouplist for the full supplementary set
   — the same derivation as the SP0 corpus's GetConnectionCredentials.
   UnixGroupIDs, so the policy engine sees identical gids. */

#include <sys/socket.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "sdbus_match.h"

#define SDBUS_MAX_GIDS 64
#define SDBUS_MAX_PENDING_FDS 16

/* one queued outbound message: its bytes and the unix fds bound to it. Keeping
   fds per-chunk is what makes SCM_RIGHTS relay correct under partial writes. */
typedef struct { unsigned char *b; int len, off; int fds[SDBUS_MAX_PENDING_FDS]; int nfds; } sdbus_outchunk;

typedef struct {
    int fd, id;
    int authed, said_hello, negotiated_fd, saw_nul;
    uid_t uid; gid_t gid; pid_t pid;
    int gids[SDBUS_MAX_GIDS]; int n_gids;
    const char *unique;                 /* ":1.N", set at Hello */
    unsigned char *in;  int in_len, in_cap;
    unsigned char *out; int out_len, out_cap;   /* scratch for auth/driver bytes */
    int pending_fds[SDBUS_MAX_PENDING_FDS]; int n_pending_fds;   /* received, awaiting a full msg */
    sdbus_outchunk *oq; int n_oq, oq_head;      /* ordered outbound queue */
    sdbus_matchset *matches;
} sdbus_conn;

/* enqueue one outbound message (its bytes are copied). fds may be NULL. */
static inline void sdbus_conn_enqueue(sdbus_conn *c, const unsigned char *b, int len,
                                      const int *fds, int nfds) {
    c->oq = realloc(c->oq, (c->n_oq + 1) * sizeof *c->oq);
    sdbus_outchunk *ch = &c->oq[c->n_oq++];
    ch->b = malloc(len); memcpy(ch->b, b, len); ch->len = len; ch->off = 0;
    ch->nfds = nfds > SDBUS_MAX_PENDING_FDS ? SDBUS_MAX_PENDING_FDS : nfds;
    for (int i = 0; i < ch->nfds; i++) ch->fds[i] = fds[i];
}

/* does the connection have unsent outbound data? */
static inline int sdbus_conn_has_out(sdbus_conn *c) { return c->oq_head < c->n_oq; }

static inline void sdbus_conn_capture_creds(sdbus_conn *c) {
    struct ucred cred;
    socklen_t len = sizeof cred;
    c->uid = (uid_t)-1; c->gid = (gid_t)-1; c->pid = 0; c->n_gids = 0;
    if (getsockopt(c->fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return;
    c->uid = cred.uid; c->gid = cred.gid; c->pid = cred.pid;

    gid_t grps[SDBUS_MAX_GIDS];
    int ng = SDBUS_MAX_GIDS;
    struct passwd *pw = getpwuid(cred.uid);
    if (pw && getgrouplist(pw->pw_name, cred.gid, grps, &ng) >= 0) {
        if (ng > SDBUS_MAX_GIDS) ng = SDBUS_MAX_GIDS;
        for (int i = 0; i < ng; i++) c->gids[i] = (int)grps[i];
        c->n_gids = ng;
    } else {
        c->gids[0] = (int)cred.gid;    /* fall back to the primary gid */
        c->n_gids = 1;
    }
}

static inline void sdbus_conn_out_append(sdbus_conn *c, const void *data, int len) {
    if (c->out_len + len > c->out_cap) {
        c->out_cap = (c->out_len + len) * 2 + 64;
        c->out = realloc(c->out, c->out_cap);
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
}

static inline void sdbus_conn_in_append(sdbus_conn *c, const void *data, int len) {
    if (c->in_len + len > c->in_cap) {
        c->in_cap = (c->in_len + len) * 2 + 64;
        c->in = realloc(c->in, c->in_cap);
    }
    memcpy(c->in + c->in_len, data, len);
    c->in_len += len;
}

/* drop the first n bytes of the inbound buffer */
static inline void sdbus_conn_in_consume(sdbus_conn *c, int n) {
    if (n >= c->in_len) { c->in_len = 0; return; }
    memmove(c->in, c->in + n, c->in_len - n);
    c->in_len -= n;
}

static inline void sdbus_conn_free_fields(sdbus_conn *c) {
    free(c->in); free(c->out);
    for (int i = c->oq_head; i < c->n_oq; i++) free(c->oq[i].b);
    free(c->oq); c->oq = NULL; c->n_oq = c->oq_head = 0;
    if (c->matches) sdbus_match_free(c->matches);
    c->in = c->out = NULL; c->matches = NULL;
}

#endif
