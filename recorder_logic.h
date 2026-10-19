#ifndef SCHEMA_RECORDER_LOGIC_H
#define SCHEMA_RECORDER_LOGIC_H

#include "recorder.h"

enum { REC_TIER_TRACE = 0, REC_TIER_MINOR = 1, REC_TIER_CRIT = 2, REC_TIER_SESSION = 3, REC_TIER_LIVE = 4 };

typedef struct { char key[64]; uint64_t ts[32]; int n; } rec_culprit_t;

static inline int rec_tier_base(const rec_event_t *e) {
    static const char *const pid1_kinds[] = { "died", "dormant", "76-excised", "watchdog-kill",
        "watchdog-abort", "start-timeout", "spawn-fail", "readiness-lost", "fuse-tripped" };
    static const char *const session_comms[] = { "kwin_wayland", "plasmashell", "ksmserver",
        "startplasma-way", "plasma_session", "Xwayland" };
    size_t i;
    switch (e->src) {
    case REC_SRC_PID1:
        for (i = 0; i < sizeof pid1_kinds / sizeof *pid1_kinds; i++)
            if (!strcmp(e->kind, pid1_kinds[i])) return e->crit ? REC_TIER_CRIT : REC_TIER_MINOR;
        return !strcmp(e->kind, "survival-enter") ? REC_TIER_MINOR : REC_TIER_TRACE;
    case REC_SRC_SESSION:
        return !strcmp(e->kind, "plasma-exit") && e->rc != 0 ? REC_TIER_SESSION : REC_TIER_TRACE;
    case REC_SRC_COREDUMP:
        for (i = 0; i < sizeof session_comms / sizeof *session_comms; i++)
            if (!strcmp(e->comm, session_comms[i])) return REC_TIER_SESSION;
        return e->subj[0] ? REC_TIER_CRIT : REC_TIER_TRACE;
    case REC_SRC_EARLYOOM:
        return !strcmp(e->kind, "kill") ? REC_TIER_MINOR : REC_TIER_TRACE;
    case REC_SRC_PSI:
        return e->tier;
    }
    return REC_TIER_TRACE;
}

static inline int rec_tier(const rec_event_t *e) {
    int b = rec_tier_base(e);
    return b > e->tier ? b : e->tier;
}

static inline void rec_ring_push(rec_shm_t *m, const rec_event_t *e) {
    uint32_t s = __atomic_load_n(&m->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&m->seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    m->ring[m->head % REC_RING] = *e;
    m->head++;
    if (m->count < REC_RING) m->count++;
    __atomic_store_n(&m->seq, s + 2, __ATOMIC_RELEASE);
}

static inline void rec_arrow_set(rec_shm_t *m, const rec_arrow_t *a) {
    uint32_t s = __atomic_load_n(&m->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&m->seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    m->arrow = *a;
    __atomic_store_n(&m->seq, s + 2, __ATOMIC_RELEASE);
}

static inline int rec_snapshot_arrow(const rec_shm_t *m, rec_arrow_t *out) {
    int i;
    for (i = 0; i < 100; i++) {
        uint32_t s1 = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1) continue;
        memcpy(out, &m->arrow, sizeof *out);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&m->seq, __ATOMIC_RELAXED) == s1) return 0;
    }
    return -1;
}

static inline int rec_snapshot_ring(const rec_shm_t *m, rec_shm_t *out) {
    int i;
    for (i = 0; i < 100; i++) {
        uint32_t s1 = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1) continue;
        memcpy(out, m, sizeof *out);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&m->seq, __ATOMIC_RELAXED) == s1) return 0;
    }
    return -1;
}

static inline int rec_culprit_count(rec_culprit_t *tab, int *ntab, int max, const char *key, uint64_t ts, uint64_t now) {
    rec_culprit_t *c = NULL;
    uint64_t floor = now > 7 * 86400 ? now - 7 * 86400 : 0;
    int i, j;
    for (i = 0; i < *ntab; i++)
        if (!strcmp(tab[i].key, key)) { c = &tab[i]; break; }
    if (!c) {
        if (ts < floor) return 0;
        if (*ntab < max) c = &tab[(*ntab)++];
        else {
            uint64_t best = 0;
            int bi = -1;
            for (i = 0; i < *ntab; i++) {
                uint64_t newest = 0;
                for (j = 0; j < tab[i].n; j++) if (tab[i].ts[j] > newest) newest = tab[i].ts[j];
                if (newest < floor) { bi = i; break; }
                if (bi < 0 || newest < best) { bi = i; best = newest; }
            }
            c = &tab[bi];
        }
        memset(c, 0, sizeof *c);
        rec_copy(c->key, sizeof c->key, key);
    }
    for (i = 0, j = 0; i < c->n; i++)
        if (c->ts[i] >= floor) c->ts[j++] = c->ts[i];
    c->n = j;
    if (c->n == 32) { memmove(c->ts, c->ts + 1, 31 * sizeof c->ts[0]); c->n--; }
    if (ts >= floor) c->ts[c->n++] = ts;
    return c->n;
}

static inline const char *rec_signame(int sig) {
    switch (sig) {
    case 4: return "SIGILL";
    case 6: return "SIGABRT";
    case 7: return "SIGBUS";
    case 8: return "SIGFPE";
    case 9: return "SIGKILL";
    case 11: return "SIGSEGV";
    case 31: return "SIGSYS";
    }
    return NULL;
}

static inline void rec_render(char *buf, size_t n, const rec_event_t *e, uint32_t recurrence) {
    time_t t = (time_t)e->ts;
    struct tm tm;
    char what[256] = "", suf[48] = "";
    const char *sn;
    int len;
    localtime_r(&t, &tm);
    switch (e->src) {
    case REC_SRC_SESSION:
        snprintf(what, sizeof what, "desktop session ended%s%s", e->msg[0] ? " \xe2\x80\x94 " : "", e->msg);
        break;
    case REC_SRC_COREDUMP:
        sn = rec_signame(e->rc);
        if (sn) snprintf(what, sizeof what, "%s crashed (%s)", e->comm, sn);
        else snprintf(what, sizeof what, "%s crashed (SIG%d)", e->comm, (int)e->rc);
        break;
    case REC_SRC_EARLYOOM:
        snprintf(what, sizeof what, "earlyoom killed %s", e->comm);
        break;
    case REC_SRC_PID1:
        snprintf(what, sizeof what, "%s %s", e->subj, e->kind);
        break;
    default:
        snprintf(what, sizeof what, "%s", e->msg);
        break;
    }
    if (recurrence >= 2) {
        uint32_t r = recurrence % 100;
        const char *o = r >= 11 && r <= 13 ? "th" : recurrence % 10 == 1 ? "st" : recurrence % 10 == 2 ? "nd" : recurrence % 10 == 3 ? "rd" : "th";
        snprintf(suf, sizeof suf, " (%u%s time this week)", (unsigned)recurrence, o);
    }
    len = snprintf(buf, n, "\xe2\x96\xb6 %02d:%02d %s", tm.tm_hour, tm.tm_min, what);
    if (len > 0 && (size_t)len < n) snprintf(buf + len, n - (size_t)len, "%s", suf);
}

static inline int rec_arrow_pick(const rec_arrow_t *cur, const rec_event_t *cand, uint64_t now) {
    int ct = rec_tier(cand);
    if (ct == REC_TIER_TRACE) return 0;
    if (cur->state == 0) return 1;
    if (cur->state == 2 && now >= cur->resolved_ts && now - cur->resolved_ts >= 36 * 3600) return 1;
    if (ct != cur->tier) return ct > cur->tier;
    return cand->ts >= cur->event_ts;
}

static inline int rec_arrow_shade(const rec_arrow_t *a, uint64_t now) {
    uint64_t age;
    if (a->state == 0) return 0;
    if (a->state == 1 || now < a->resolved_ts) return 1;
    age = now - a->resolved_ts;
    if (age < 12 * 3600) return 1;
    if (age < 36 * 3600) return 2;
    return 0;
}

typedef struct { int32_t pid, ppid; char comm[32]; uint64_t ticks; } rec_proc_t;
typedef struct { char comm[32]; int count; uint64_t ticks; int32_t top_pid; } rec_hog_t;
typedef struct {
    uint8_t active, kind;
    uint64_t start_mono, last_fire_mono, start_wall;
    rec_hog_t top;
    char chain[96], cmdline[96];
    uint8_t overlapped;
} rec_episode_t;

static inline int rec_hogs(const rec_proc_t *a, int na, const rec_proc_t *b, int nb, rec_hog_t *out, int max) {
    rec_hog_t g[512];
    int i, j, k, n = 0;
    for (i = 0; i < nb; i++) {
        uint64_t d;
        const rec_proc_t *pa = NULL;
        for (j = 0; j < na; j++)
            if (a[j].pid == b[i].pid && !strcmp(a[j].comm, b[i].comm)) { pa = &a[j]; break; }
        if (!pa || b[i].ticks <= pa->ticks) continue;
        d = b[i].ticks - pa->ticks;
        for (k = 0; k < n; k++) if (!strcmp(g[k].comm, b[i].comm)) break;
        if (k == n) {
            if (n == 512) continue;
            memset(&g[n], 0, sizeof g[n]);
            rec_copy(g[n].comm, sizeof g[n].comm, b[i].comm);
            n++;
        }
        g[k].count++;
        g[k].ticks += d;
        if (b[i].pid > g[k].top_pid) g[k].top_pid = b[i].pid;
    }
    for (i = 1; i < n; i++) {
        rec_hog_t t = g[i];
        for (j = i - 1; j >= 0 && g[j].ticks < t.ticks; j--) g[j + 1] = g[j];
        g[j + 1] = t;
    }
    if (n > max) n = max;
    for (i = 0; i < n; i++) out[i] = g[i];
    return n;
}

static inline void rec_episode_fire(rec_episode_t *ep, int kind, uint64_t mono, uint64_t wall) {
    if (!ep->active) {
        memset(ep, 0, sizeof *ep);
        ep->active = 1;
        ep->kind = (uint8_t)kind;
        ep->start_mono = mono;
        ep->start_wall = wall;
    }
    ep->last_fire_mono = mono;
}

static inline int rec_episode_check(rec_episode_t *ep, uint64_t mono) {
    return ep->active && mono - ep->last_fire_mono >= 30;
}

static inline int rec_episode_arrow(const rec_episode_t *ep, uint64_t end_mono) {
    if (end_mono > ep->last_fire_mono) end_mono = ep->last_fire_mono;
    return ep->overlapped || end_mono - ep->start_mono >= 120;
}

static inline void rec_episode_text(const rec_episode_t *ep, uint64_t end_mono, char *buf, size_t n) {
    unsigned long long mins = end_mono > ep->start_mono ? (end_mono - ep->start_mono) / 60 : 0;
    const char *what = ep->kind == 2 ? "memory pressure" : "CPU pinned";
    if (ep->top.count > 1)
        snprintf(buf, n, "%s %llu min by %s \xc3\x97%d", what, mins, ep->top.comm, ep->top.count);
    else
        snprintf(buf, n, "%s %llu min by %s", what, mins, ep->top.comm);
}

static inline int rec_correlate(const rec_shm_t *m, const rec_episode_t *ep, const rec_event_t *x, char *why, size_t n) {
    uint64_t lo = x->ts > 20 ? x->ts - 20 : 0;
    const rec_event_t *crash = NULL, *oom = NULL, *svc = NULL;
    uint32_t i;
    for (i = 0; i < m->count; i++) {
        const rec_event_t *e = &m->ring[(m->head - 1 - i) % REC_RING];
        if (e->ts < lo || e->ts > x->ts) continue;
        if (e->src == REC_SRC_COREDUMP) { if (!crash && rec_tier_base(e) != REC_TIER_TRACE) crash = e; }
        else if (e->src == REC_SRC_EARLYOOM) { if (!oom && !strcmp(e->kind, "kill")) oom = e; }
        else if (e->src == REC_SRC_PID1) { if (!svc && rec_tier_base(e) != REC_TIER_TRACE) svc = e; }
    }
    if (crash) {
        const char *sn = rec_signame(crash->rc);
        if (sn) snprintf(why, n, "%s crashed (%s)", crash->comm, sn);
        else snprintf(why, n, "%s crashed (SIG%d)", crash->comm, (int)crash->rc);
        return 1;
    }
    if (oom) { snprintf(why, n, "earlyoom killed %s", oom->comm); return 1; }
    if (svc) { snprintf(why, n, "%s %s", svc->subj, svc->kind); return 1; }
    if (ep->kind && ep->start_wall <= x->ts &&
        (ep->active || ep->start_wall + (ep->last_fire_mono - ep->start_mono) >= lo)) {
        rec_episode_text(ep, ep->last_fire_mono, why, n);
        return 1;
    }
    return 0;
}

#endif
