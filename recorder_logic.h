#ifndef SCHEMA_RECORDER_LOGIC_H
#define SCHEMA_RECORDER_LOGIC_H

#include "recorder.h"

enum { REC_TIER_TRACE = 0, REC_TIER_MINOR = 1, REC_TIER_CRIT = 2, REC_TIER_SESSION = 3, REC_TIER_LIVE = 4 };

typedef struct { char key[64]; uint64_t ts[32]; int n; } rec_culprit_t;

static inline int rec_tier(const rec_event_t *e) {
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

static inline void rec_ring_push(rec_shm_t *m, const rec_event_t *e) {
    uint32_t s = __atomic_load_n(&m->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&m->seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    m->ring[m->head % REC_RING] = *e;
    m->head++;
    if (m->count < REC_RING) m->count++;
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
        if (*ntab < max) c = &tab[(*ntab)++];
        else c = &tab[0];
        memset(c, 0, sizeof *c);
        rec_copy(c->key, sizeof c->key, key);
    }
    for (i = 0, j = 0; i < c->n; i++)
        if (c->ts[i] >= floor) c->ts[j++] = c->ts[i];
    c->n = j;
    if (c->n == 32) { memmove(c->ts, c->ts + 1, 31 * sizeof c->ts[0]); c->n--; }
    c->ts[c->n++] = ts;
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
    if (cur->state == 0) return 1;
    if (cur->state == 2 && now >= cur->resolved_ts && now - cur->resolved_ts >= 36 * 3600) return 1;
    if (ct != cur->tier) return ct > cur->tier;
    return cand->ts > cur->event_ts;
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

#endif
