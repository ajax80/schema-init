#ifndef REEXEC_STATE_H
#define REEXEC_STATE_H

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include "service.h"

#define MAX_EVICTIONS     16

typedef struct {
    pid_t  pid;
    char   cgroup[128];
    time_t deadline;
} eviction_t;

/* The runtime half of service_t: everything a running PID 1 learned that a
 * fresh .svc parse cannot reproduce. A config reload carries these into the
 * shadow table and a re-exec carries them into the new image, both from this
 * one list. A field missing here is lost on reload and on re-exec alike. */
#define SVC_RUNTIME_FIELDS(X) \
    X(PID,  child_pid)           \
    X(INST, inst)                \
    X(INT,  restart_count)       \
    X(U8,   dormant_count)       \
    X(TS,   dormant_until)       \
    X(TIME, last_start)          \
    X(TIME, start_time)          \
    X(TS,   stable_time)         \
    X(PID,  failsafe_pid)        \
    X(TS,   failsafe_start)      \
    X(TS,   last_pet)            \
    X(INT,  wd_armed_sec)        \
    X(TS,   wd_abort_at)         \
    X(INT,  ready_path_verified) \
    X(U64,  ready_stale_dev)     \
    X(U64,  ready_stale_ino)     \
    X(TS,   ready_stale_ctime)   \
    X(INT,  notify_ready)        \
    X(STR,  notify_status)       \
    X(INT,  ctl_killed)          \
    X(INT,  exit_status)         \
    X(INT,  term_signal)         \
    X(INT,  core_dumped)         \
    X(INT,  has_exited)          \
    X(TS,   timer_next)          \
    X(TS,   spawn_time_mono)     \
    X(TS,   boot_spawn)          \
    X(TS,   boot_ready)          \
    X(INT,  boot_how)            \
    X(STR,  cgroup_path)         \
    X(INT,  is_frozen)           \
    X(INT,  fork_state)          \
    X(TS,   fork_wait)

/* SVC_NO_RESTART is the one flag PID 1 changes at runtime: schema-ctl stop
 * sets it to hold a service down, start clears it. The live bit wins over the
 * fresh parse, or a stopped service respawns after a reload or re-exec. */
static inline void svc_runtime_copy(service_t *dst, const service_t *src) {
#define X(kind, f) memcpy(&dst->f, &src->f, sizeof dst->f);
    SVC_RUNTIME_FIELDS(X)
#undef X
    dst->flags = (dst->flags & ~SVC_NO_RESTART) | (src->flags & SVC_NO_RESTART);
}

/* A run-once timer (on_boot_sec, no interval) clears SVC_TIMER when it
 * completes. A fresh parse has the flag back; carry the terminal fact or the
 * expired timer_next fires it again. live_done: the live table had cleared it. */
static inline void svc_carry_timer_done(service_t *fresh, int live_done) {
    if (live_done && (fresh->flags & SVC_TIMER) &&
        !(fresh->flags & SVC_TIMER_CALENDAR) && fresh->timer_interval_sec <= 0)
        fresh->flags &= ~SVC_TIMER;
}

/* ── State blob ──────────────────────────────────────────────────────────
 * What the old image hands the new one across execve: versioned text, one
 * record per line, key=value tokens, values percent-escaped. A reader takes
 * any format <= its own, ignores unknown records and keys, defaults missing
 * ones, and rejects a blob without its "end" line. Pure: FILE* in and out. */

#define REEXEC_FORMAT 1

typedef struct {
    char            version[64];
    char            argv0[256];
    struct timespec init_start;
    int             under_pressure;
    uint64_t        last_stall_ms;
    uint64_t        last_reclaim_ms;
    uint64_t        nofile_soft;     /* soft RLIMIT_NOFILE the kernel gave PID 1 at boot */
    int             fd_ctl, fd_notify, fd_watchdog, fd_client, fd_oldexe;
} reexec_global_t;

/* one service as the blob carries it: identity + live SVC_TIMER bit + the
 * runtime fields, in a service_t so svc_runtime_copy can overlay it */
typedef struct {
    char      name[64];
    uint32_t  hash;
    int       timer;
    service_t rt;
} reexec_svc_t;

static inline void rx_escape(FILE *out, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p <= 0x20 || *p >= 0x7f || *p == '%' || *p == '=')
            fprintf(out, "%%%02X", *p);
        else
            fputc(*p, out);
    }
}

/* decode into dst (size n); -1 on a bad escape or overflow */
static inline int rx_unescape(const char *v, char *dst, size_t n) {
    size_t o = 0;
    for (; *v; v++) {
        unsigned c = (unsigned char)*v;
        if (c == '%') {
            unsigned hi, lo;
            char h[3] = { v[1], v[1] ? v[2] : 0, 0 };
            if (!isxdigit((unsigned char)h[0]) || !isxdigit((unsigned char)h[1])) return -1;
            hi = isdigit((unsigned char)h[0]) ? h[0] - '0' : (tolower((unsigned char)h[0]) - 'a' + 10);
            lo = isdigit((unsigned char)h[1]) ? h[1] - '0' : (tolower((unsigned char)h[1]) - 'a' + 10);
            c = hi * 16 + lo;
            v += 2;
        }
        if (o + 1 >= n) return -1;
        dst[o++] = (char)c;
    }
    dst[o] = '\0';
    return 0;
}

static inline int rx_ll(const char *v, long long *out) {
    char *end;
    errno = 0;
    long long x = strtoll(v, &end, 10);
    if (errno || end == v || *end) return -1;
    *out = x;
    return 0;
}

static inline void rx_put_PID(FILE *o, const char *k, const pid_t *v)   { fprintf(o, " %s=%d", k, (int)*v); }
static inline void rx_put_INT(FILE *o, const char *k, const int *v)     { fprintf(o, " %s=%d", k, *v); }
static inline void rx_put_U8(FILE *o, const char *k, const uint8_t *v)  { fprintf(o, " %s=%u", k, (unsigned)*v); }
static inline void rx_put_U64(FILE *o, const char *k, const uint64_t *v) { fprintf(o, " %s=%" PRIu64, k, *v); }
static inline void rx_put_TIME(FILE *o, const char *k, const time_t *v) { fprintf(o, " %s=%lld", k, (long long)*v); }
static inline void rx_put_TS(FILE *o, const char *k, const struct timespec *v) {
    fprintf(o, " %s=%lld.%09ld", k, (long long)v->tv_sec, (long)v->tv_nsec);
}
static inline void rx_put_STR(FILE *o, const char *k, const void *v)    { fprintf(o, " %s=", k); rx_escape(o, (const char *)v); }
static inline void rx_put_INST(FILE *o, const char *k, const schema_instance_t *v) {
    fprintf(o, " %s=%u,%u,%u,%u,%" PRIu32 ",%" PRIu32, k, v->state, v->prev_state,
            v->weight, v->target_c, v->pid, v->flags);
}

static inline int rx_get_PID(const char *v, pid_t *d, size_t n)  { long long x; (void)n; if (rx_ll(v, &x) || x < 0 || x > INT32_MAX) return -1; *d = (pid_t)x; return 0; }
static inline int rx_get_INT(const char *v, int *d, size_t n)    { long long x; (void)n; if (rx_ll(v, &x) || x < INT32_MIN || x > INT32_MAX) return -1; *d = (int)x; return 0; }
static inline int rx_get_U8(const char *v, uint8_t *d, size_t n) { long long x; (void)n; if (rx_ll(v, &x) || x < 0 || x > 255) return -1; *d = (uint8_t)x; return 0; }
static inline int rx_get_U64(const char *v, uint64_t *d, size_t n) {
    unsigned long long x; char *end; (void)n;
    if (*v < '0' || *v > '9') return -1;
    errno = 0;
    x = strtoull(v, &end, 10);
    if (errno || *end) return -1;
    *d = (uint64_t)x;
    return 0;
}
static inline int rx_get_TIME(const char *v, time_t *d, size_t n){ long long x; (void)n; if (rx_ll(v, &x)) return -1; *d = (time_t)x; return 0; }
static inline int rx_get_TS(const char *v, struct timespec *d, size_t n) {
    long long sec, ns; char *end; (void)n;
    errno = 0;
    sec = strtoll(v, &end, 10);
    if (errno || end == v || *end != '.') return -1;
    const char *f = end + 1;
    if (strlen(f) != 9 || rx_ll(f, &ns) || ns < 0) return -1;
    d->tv_sec = (time_t)sec; d->tv_nsec = (long)ns;
    return 0;
}
static inline int rx_get_STR(const char *v, void *d, size_t n)   { return rx_unescape(v, (char *)d, n); }
static inline int rx_get_INST(const char *v, schema_instance_t *d, size_t n) {
    unsigned st, pv, w, t; unsigned long pid, fl; char extra; (void)n;
    if (sscanf(v, "%u,%u,%u,%u,%lu,%lu%c", &st, &pv, &w, &t, &pid, &fl, &extra) != 6) return -1;
    if (st > 255 || pv > 255 || w > 255 || t > 255 || pid > UINT32_MAX || fl > UINT32_MAX) return -1;
    d->state = (uint8_t)st; d->prev_state = (uint8_t)pv; d->weight = (uint8_t)w;
    d->target_c = (uint8_t)t; d->pid = (uint32_t)pid; d->flags = (uint32_t)fl;
    return 0;
}

static inline void state_write(FILE *out, const reexec_global_t *g,
                               const service_t *svcs, int nsvc,
                               const eviction_t *ev, int nev) {
    fprintf(out, "schema-init-state %d\n", REEXEC_FORMAT);
    fprintf(out, "global");
    rx_put_STR(out, "version", g->version);
    rx_put_STR(out, "argv0", g->argv0);
    rx_put_TS(out, "init_start", &g->init_start);
    rx_put_INT(out, "under_pressure", &g->under_pressure);
    fprintf(out, " last_stall_ms=%" PRIu64 " last_reclaim_ms=%" PRIu64 " nofile_soft=%" PRIu64 "\n",
            g->last_stall_ms, g->last_reclaim_ms, g->nofile_soft);
    fprintf(out, "fd ctl=%d notify=%d watchdog=%d client=%d oldexe=%d\n",
            g->fd_ctl, g->fd_notify, g->fd_watchdog, g->fd_client, g->fd_oldexe);
    for (int i = 0; i < nsvc; i++) {
        const service_t *s = &svcs[i];
        int timer = (s->flags & SVC_TIMER) != 0;
        fprintf(out, "svc");
        rx_put_STR(out, "name", s->name);
        fprintf(out, " hash=%" PRIu32, s->content_hash);
        rx_put_INT(out, "timer", &timer);
        int no_restart = (s->flags & SVC_NO_RESTART) != 0;
        rx_put_INT(out, "no_restart", &no_restart);
#define X(kind, f) rx_put_##kind(out, #f, &s->f);
        SVC_RUNTIME_FIELDS(X)
#undef X
        fputc('\n', out);
    }
    for (int i = 0; i < nev; i++) {
        fprintf(out, "evict pid=%d deadline=%lld cgroup=", (int)ev[i].pid, (long long)ev[i].deadline);
        rx_escape(out, ev[i].cgroup);
        fputc('\n', out);
    }
    fprintf(out, "end\n");
}

/* Returns 0 and fills the outputs, or -1 with a reason in err. svcs holds up
 * to maxsvc entries; evictions up to MAX_EVICTIONS. */
static inline int state_parse(FILE *in, reexec_global_t *g,
                              reexec_svc_t *svcs, int maxsvc, int *nsvc,
                              eviction_t *ev, int *nev,
                              char *err, size_t errsz) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int lineno = 0, ended = 0, rc = -1;

    memset(g, 0, sizeof *g);
    g->fd_ctl = g->fd_notify = g->fd_watchdog = g->fd_client = g->fd_oldexe = -1;
    *nsvc = 0;
    *nev = 0;

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); goto out; } while (0)
    while ((len = getline(&line, &cap, in)) > 0) {
        char *save = NULL, *rec, *tok;
        lineno++;
        if (line[len - 1] == '\n') line[--len] = '\0';
        if (ended) FAIL("line %d: data after end", lineno);
        if (lineno == 1) {
            int ver; char extra;
            if (sscanf(line, "schema-init-state %d%c", &ver, &extra) != 1)
                FAIL("not a schema-init state blob");
            if (ver < 1 || ver > REEXEC_FORMAT)
                FAIL("state format %d, this image reads up to %d", ver, REEXEC_FORMAT);
            continue;
        }
        rec = strtok_r(line, " ", &save);
        if (!rec) continue;
        if (!strcmp(rec, "end")) { ended = 1; continue; }

        reexec_svc_t *sv = NULL;
        eviction_t *e = NULL;
        if (!strcmp(rec, "svc")) {
            if (*nsvc >= maxsvc) FAIL("line %d: more than %d services", lineno, maxsvc);
            sv = &svcs[*nsvc];
            memset(sv, 0, sizeof *sv);
        } else if (!strcmp(rec, "evict")) {
            if (*nev >= MAX_EVICTIONS) FAIL("line %d: more than %d evictions", lineno, MAX_EVICTIONS);
            e = &ev[*nev];
            memset(e, 0, sizeof *e);
        } else if (strcmp(rec, "global") && strcmp(rec, "fd")) {
            continue;                                   /* unknown record: newer writer */
        }

        while ((tok = strtok_r(NULL, " ", &save))) {
            char *eq = strchr(tok, '=');
            int bad = 0;
            long long x;
            if (!eq) FAIL("line %d: token without '='", lineno);
            *eq = '\0';
            const char *k = tok, *v = eq + 1;
            if (sv) {
                if (!strcmp(k, "name"))       bad = rx_unescape(v, sv->name, sizeof sv->name);
                else if (!strcmp(k, "hash"))  { bad = rx_ll(v, &x) || x < 0 || x > UINT32_MAX; if (!bad) sv->hash = (uint32_t)x; }
                else if (!strcmp(k, "timer")) bad = rx_get_INT(v, &sv->timer, 0);
                else if (!strcmp(k, "no_restart")) {
                    int nr = 0;
                    bad = rx_get_INT(v, &nr, 0);
                    if (nr) sv->rt.flags |= SVC_NO_RESTART;
                }
#define X(kind, f) else if (!strcmp(k, #f)) bad = rx_get_##kind(v, &sv->rt.f, sizeof sv->rt.f);
                SVC_RUNTIME_FIELDS(X)
#undef X
            } else if (e) {
                if (!strcmp(k, "pid"))           bad = rx_get_PID(v, &e->pid, 0);
                else if (!strcmp(k, "deadline")) bad = rx_get_TIME(v, &e->deadline, 0);
                else if (!strcmp(k, "cgroup"))   bad = rx_unescape(v, e->cgroup, sizeof e->cgroup);
            } else if (!strcmp(rec, "global")) {
                if (!strcmp(k, "version"))             bad = rx_unescape(v, g->version, sizeof g->version);
                else if (!strcmp(k, "argv0"))          bad = rx_unescape(v, g->argv0, sizeof g->argv0);
                else if (!strcmp(k, "init_start"))     bad = rx_get_TS(v, &g->init_start, 0);
                else if (!strcmp(k, "under_pressure")) bad = rx_get_INT(v, &g->under_pressure, 0);
                else if (!strcmp(k, "last_stall_ms"))  { bad = rx_ll(v, &x) || x < 0; if (!bad) g->last_stall_ms = (uint64_t)x; }
                else if (!strcmp(k, "last_reclaim_ms")){ bad = rx_ll(v, &x) || x < 0; if (!bad) g->last_reclaim_ms = (uint64_t)x; }
                else if (!strcmp(k, "nofile_soft"))    { bad = rx_ll(v, &x) || x < 0; if (!bad) g->nofile_soft = (uint64_t)x; }
            } else {
                int *fd = !strcmp(k, "ctl") ? &g->fd_ctl : !strcmp(k, "notify") ? &g->fd_notify :
                          !strcmp(k, "watchdog") ? &g->fd_watchdog : !strcmp(k, "client") ? &g->fd_client :
                          !strcmp(k, "oldexe") ? &g->fd_oldexe : NULL;
                if (fd) bad = rx_get_INT(v, fd, 0) || *fd < -1;
            }
            if (bad) FAIL("line %d: bad value for %s", lineno, k);
        }
        if (sv) {
            if (!sv->name[0]) FAIL("line %d: svc without name", lineno);
            (*nsvc)++;
        }
        if (e) (*nev)++;
    }
    if (lineno == 0) FAIL("empty state blob");
    if (!ended) FAIL("truncated state blob (no end line)");
    rc = 0;
out:
#undef FAIL
    free(line);
    return rc;
}

/* The blob against a fresh parse of the .svc files. Shared by the dry-run
 * child, the old image's commit-time re-check and the new image, so all three
 * refuse the same things: a duplicate name, a running service with no .svc
 * (it would be lost), and the reload integrity rule (a .svc changed since
 * boot waits for the next boot). Returns 0 or -1 with err. */
static inline int reexec_compare(const reexec_svc_t *b, int nb,
                                 const service_t *loaded, int nl,
                                 char *err, size_t errsz) {
    for (int i = 0; i < nb; i++) {
        int j;
        for (j = 0; j < i; j++)
            if (!strcmp(b[i].name, b[j].name)) {
                snprintf(err, errsz, "'%.63s' appears twice in the state blob", b[i].name);
                return -1;
            }
        for (j = 0; j < nl; j++)
            if (!strcmp(b[i].name, loaded[j].name)) break;
        if (j == nl) {
            if (b[i].rt.child_pid > 0 || b[i].rt.failsafe_pid > 0) {
                snprintf(err, errsz, "'%.63s' is running (pid %d) but has no .svc on disk; "
                         "run schema-ctl reload --evict first", b[i].name,
                         (int)(b[i].rt.child_pid > 0 ? b[i].rt.child_pid : b[i].rt.failsafe_pid));
                return -1;
            }
            continue;
        }
        if (loaded[j].content_hash != b[i].hash && loaded[j].content_hash != 0) {
            snprintf(err, errsz, "'%.63s' modified since boot; a changed .svc takes effect "
                     "at next boot, so re-exec is refused", b[i].name);
            return -1;
        }
    }
    return 0;
}

#endif
