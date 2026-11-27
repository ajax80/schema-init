#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rail.h"
#include "rail_glyphs.h"

#define RST "\033[0m"

typedef struct { char *p; size_t sz, n; } wbuf_t;

static void put(wbuf_t *w, const char *fmt, ...) {
    va_list ap;
    int k;
    if (w->n >= w->sz) return;
    va_start(ap, fmt);
    k = vsnprintf(w->p + w->n, w->sz - w->n, fmt, ap);
    va_end(ap);
    if (k > 0) w->n = w->n + (size_t)k < w->sz ? w->n + (size_t)k : w->sz - 1;
}

/* one code point from s, its length in *len; invalid UTF-8 is one byte, -1 */
static long utf8_cp(const unsigned char *s, int *len) {
    long c;
    int n, i;
    if (s[0] < 0x80) { *len = 1; return s[0]; }
    if ((s[0] & 0xe0) == 0xc0) { c = s[0] & 0x1f; n = 2; }
    else if ((s[0] & 0xf0) == 0xe0) { c = s[0] & 0x0f; n = 3; }
    else if ((s[0] & 0xf8) == 0xf0) { c = s[0] & 0x07; n = 4; }
    else { *len = 1; return -1; }
    for (i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) { *len = 1; return -1; }
        c = (c << 6) | (s[i] & 0x3f);
    }
    *len = n;
    return c;
}

/* Names, reasons and hostnames come from outside: every code point is shown
 * as itself only if the active font can draw it, else '?'. maxcols < 0 = no
 * limit; pad fills to maxcols. Returns columns written. */
static int clean(wbuf_t *w, const char *in, int fontmode, int maxcols, int pad) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    const unsigned char *s = (const unsigned char *)in;
    int cols = 0, total = 0, len;

    if (maxcols >= 0)
        for (const unsigned char *q = s; *q; q += len, total++) utf8_cp(q, &len);
    for (; *s; s += len) {
        long cp = utf8_cp(s, &len);
        if (maxcols >= 0 && total > maxcols && cols == maxcols - 1) {
            put(w, "%s", ab->trunc);
            return ++cols;
        }
        if (cp >= 0x20 && cp < 0x7f) put(w, "%c", (int)cp);
        else if (fontmode && cp >= 0xa0 && schema_font_has((uint32_t)cp)) put(w, "%.*s", len, (const char *)s);
        else if (!fontmode && cp == 0xb7) put(w, "-");
        else if (!fontmode && cp == 0x2026) put(w, "~");
        else put(w, "?");
        cols++;
    }
    while (pad && maxcols >= 0 && cols < maxcols) { put(w, " "); cols++; }
    return cols;
}

int rail_name_cols(char *out, size_t sz, const char *name, int fontmode) {
    wbuf_t w = { out, sz, 0 };
    out[0] = '\0';
    clean(&w, name, fontmode, RAIL_NAME_COLS, 1);
    return (int)w.n;
}

static const struct { const char *label, *fg, *bg; int glyph; } CH[] = {
    [CHIP_NONE]    = { "",        "90",   "30;47",  0 },
    [CHIP_ONLINE]  = { "ONLINE",  "32",   "30;42",  0 },
    [CHIP_PERFECT] = { "PERFECT", "35",   "30;45",  0 },
    [CHIP_DONE]    = { "DONE",    "32",   "30;42",  0 },
    [CHIP_SPAWN]   = { "SPAWN",   "2;32", "2;30;42", 1 },
    [CHIP_WAITING] = { "WAITING", "2;32", "2;30;42", 1 },
    [CHIP_RETRY]   = { "RETRY",   "33",   "30;43",  2 },
    [CHIP_DORMANT] = { "DORMANT", "34",   "30;44",  4 },
    [CHIP_CUT]     = { "CUT",     "31",   "30;41",  3 },
    [CHIP_STOPPED] = { "STOPPED", "34",   "30;44",  4 },
    /* not 100: the Linux console has no bright backgrounds and draws it black */
    [CHIP_EXITED]  = { "EXITED",  "90",   "30;47",  4 },
    [CHIP_KILLED]  = { "KILLED",  "31",   "30;41",  3 },
    [CHIP_STUCK]   = { "STUCK",   "31",   "30;41",  3 },
    [CHIP_TIMER]   = { "TIMER",   "36",   "30;46",  0 },
    [CHIP_FAILED]  = { "FAILED",  "33",   "30;43",  2 },
};

size_t rail_fmt_line(char *out, size_t sz, const rail_line_t *l, int fontmode) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    const char *glyphs[] = { ab->g_on, ab->g_start, ab->g_warn, ab->g_cut, ab->g_stop };
    wbuf_t w = { out, sz, 0 };
    char chip[24];

    out[0] = '\0';
    if (l->chip == CHIP_RETRY && l->retry_n > 0) snprintf(chip, sizeof chip, "RETRY %d", l->retry_n);
    else snprintf(chip, sizeof chip, "%s", CH[l->chip].label);
    put(&w, "\033[%sm%s%s" RST " \033[1;37m", CH[l->chip].fg, l->runtime ? ab->branch_rt : ab->branch,
        glyphs[CH[l->chip].glyph]);
    clean(&w, l->name ? l->name : "", fontmode, RAIL_NAME_COLS, 1);
    put(&w, RST " \033[%sm %-8s" RST "  ", CH[l->chip].bg, chip);
    if (l->reason[0]) {
        put(&w, "\033[%sm", CH[l->chip].fg);
        clean(&w, l->reason, fontmode, -1, 0);
    } else if (l->runtime) {
        time_t t = (time_t)(l->t_ms / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        put(&w, "\033[90m%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else
        put(&w, "\033[90m+%lld.%02llds", (long long)(l->t_ms / 1000), (long long)(l->t_ms % 1000 / 10));
    put(&w, RST "\n");
    return w.n;
}

size_t rail_fmt_banner(char *out, size_t sz, const char *word, const char *version, const char *host, int nsvc,
                       int fontmode) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    wbuf_t w = { out, sz, 0 };
    int halt = !strcmp(word, "HALT");

    out[0] = '\0';
    put(&w, "\033[35m%s" RST " \033[95mS C H E M A\033[36m//\033[%sm", ab->banner_l, halt ? "91" : "92");
    for (const char *c = word; *c; c++) put(&w, c[1] ? "%c " : "%c", *c);
    put(&w, RST " \033[35m%s" RST, ab->banner_r);
    if (version) {
        put(&w, "  \033[90m%s", version[0] == 'v' ? "" : "v");
        clean(&w, version, fontmode, -1, 0);
        put(&w, " %s ", ab->sep);
        clean(&w, host ? host : "?", fontmode, -1, 0);
        put(&w, " %s %d services" RST, ab->sep, nsvc);
    }
    put(&w, "\n");
    if (!halt) put(&w, "\033[35m%s" RST "\n", ab->track_v);
    return w.n;
}

size_t rail_fmt_finish(char *out, size_t sz, const rail_counts_t *c, int64_t settled_ms, int timed_out, int fontmode) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    const struct { int n; const char *label, *bg; } b[] = {
        { c->online, "ONLINE", "30;42" }, { c->waiting, "WAITING", "30;47" }, { c->friction, "FRICTION", "30;43" },
        { c->cut, "CUT", "30;41" }, { c->starting, "STARTING", "30;43" },
    };
    wbuf_t w = { out, sz, 0 };

    out[0] = '\0';
    put(&w, "\033[35m%s" RST, ab->finish);
    for (size_t i = 0; i < sizeof b / sizeof *b; i++)
        if (b[i].n) put(&w, " \033[%sm %d %s " RST, b[i].bg, b[i].n, b[i].label);
    if (timed_out) put(&w, "  \033[33mrail still starting after %llds" RST "\n", (long long)(settled_ms / 1000));
    else put(&w, "  \033[36mrail settled in %lld.%02llds" RST "\n", (long long)(settled_ms / 1000),
             (long long)(settled_ms % 1000 / 10));
    return w.n;
}

size_t rail_fmt_shutdown_end(char *out, size_t sz, int64_t ms, int reboot, int fontmode) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    wbuf_t w = { out, sz, 0 };
    out[0] = '\0';
    put(&w, "\033[35m%s" RST " \033[36mall down in %lld.%llds %s %s" RST "\n", ab->finish, (long long)(ms / 1000),
        (long long)(ms % 1000 / 100), ab->sep, reboot ? "rebooting" : "powering off");
    return w.n;
}

size_t rail_fmt_note(char *out, size_t sz, const char *text, int fontmode) {
    const struct rail_alphabet *ab = fontmode ? &rail_font_ab : &rail_ascii_ab;
    wbuf_t w = { out, sz, 0 };
    out[0] = '\0';
    put(&w, "\033[90m%s ", ab->branch);
    clean(&w, text, fontmode, -1, 0);
    put(&w, RST "\n");
    return w.n;
}

/* ── the state machine ─────────────────────────────────────────────────── */

#include "schema.h"

enum { K_CHIP, K_QUIET, K_READY, K_SPAWN, K_DIED, K_END };

/* Every event the source emits (tests/rail_events.txt) has an entry. "x*" matches
 * names built at runtime; otherwise the event's first word must match. A quiet
 * entry for anything that sounds like a failure says why it stays quiet. */
static const struct { const char *key; int kind; rail_chip_t chip; const char *why; } EV[] = {
    { "76-excised",          K_CHIP,  CHIP_CUT, "" },
    { "76-no-restart",       K_END,   CHIP_NONE, "" },
    { "76-restart-policy",   K_END,   CHIP_NONE, "" },
    { "bind-reclassified",   K_QUIET, CHIP_NONE, "bound-to bookkeeping" },
    { "bind-start",          K_QUIET, CHIP_NONE, "its spawn and ready lines follow" },
    { "bind-stop",           K_QUIET, CHIP_NONE, "bind-stopped follows" },
    { "bind-stopped",        K_CHIP,  CHIP_STOPPED, "" },
    { "bind-target-missing", K_CHIP,  CHIP_WAITING, "" },
    { "bind-wait",           K_CHIP,  CHIP_WAITING, "" },
    { "blocked",             K_QUIET, CHIP_NONE, "waiting on its dependencies; counted as WAITING" },
    { "breaker-tripped",     K_CHIP,  CHIP_CUT, "" },
    { "bus-name-ready",      K_READY, CHIP_NONE, "" },
    { "card-*",              K_CHIP,  CHIP_FAILED, "" },
    { "card-done",           K_QUIET, CHIP_NONE, "a recovery card finished" },
    { "card-failed",         K_CHIP,  CHIP_FAILED, "" },
    { "card-fork-fail",      K_CHIP,  CHIP_FAILED, "" },
    { "card-no-fallback",    K_CHIP,  CHIP_CUT, "" },
    { "card-timeout",        K_CHIP,  CHIP_FAILED, "" },
    { "condition-skip",      K_QUIET, CHIP_NONE, "condition false by design; counted as WAITING" },
    { "ctl-restart",         K_QUIET, CHIP_NONE, "operator restart; its spawn and ready lines follow" },
    { "died",                K_DIED,  CHIP_NONE, "with a restart coming, the retry line carries it; else CUT" },
    { "dormant",             K_CHIP,  CHIP_DORMANT, "" },
    { "dormant-wake",        K_QUIET, CHIP_NONE, "back from DORMANT; its spawn line follows" },
    { "dynamic-uid-exhausted", K_CHIP, CHIP_CUT, "" },
    { "failsafe-done",       K_QUIET, CHIP_NONE, "the failsafe command finished" },
    { "failsafe-exec",       K_QUIET, CHIP_NONE, "the death that started it is already shown" },
    { "failsafe-fork-fail",  K_CHIP,  CHIP_FAILED, "" },
    { "failsafe-timeout",    K_CHIP,  CHIP_FAILED, "" },
    { "fallback-down",       K_CHIP,  CHIP_CUT, "" },
    { "fallback-up",         K_QUIET, CHIP_NONE, "the stand-in's spawn and ready lines follow" },
    { "fdstore-refused",     K_CHIP,  CHIP_FAILED, "" },
    { "forked",              K_QUIET, CHIP_NONE, "process bookkeeping" },
    { "freeze",              K_QUIET, CHIP_NONE, "pressure handling, shown on schema-board" },
    { "freeze-sig",          K_QUIET, CHIP_NONE, "pressure handling, shown on schema-board" },
    { "friction",            K_QUIET, CHIP_NONE, "a state step between a death and its retry or cut, which are shown" },
    { "fuse-cmd-exec",       K_QUIET, CHIP_NONE, "fuse-tripped is shown" },
    { "fuse-tripped",        K_CHIP,  CHIP_CUT, "" },
    { "killed-SIG*",         K_QUIET, CHIP_NONE, "the died / 76-* event that follows carries the outcome" },
    { "limits-mismatch",     K_QUIET, CHIP_NONE, "a cgroup limits note; the service runs" },
    { "listen-fail",         K_CHIP,  CHIP_CUT, "" },
    { "main-pid",            K_QUIET, CHIP_NONE, "readiness bookkeeping" },
    { "main-pid-no-cgroup",  K_CHIP,  CHIP_CUT, "" },
    { "main-pid-rejected",   K_QUIET, CHIP_NONE, "a refused MAINPID=; main-pid-timeout shows if none comes" },
    { "main-pid-timeout",    K_CHIP,  CHIP_CUT, "" },
    { "notify-ready",        K_READY, CHIP_NONE, "" },
    { "oneshot-done",        K_CHIP,  CHIP_DONE, "" },
    { "path-done",           K_QUIET, CHIP_NONE, "the activation finished cleanly" },
    { "path-failed",         K_CHIP,  CHIP_FAILED, "" },
    { "path-fire",           K_CHIP,  CHIP_TIMER, "" },
    { "path-trigger-limit",  K_CHIP,  CHIP_CUT, "" },
    { "pid_file-missing",    K_QUIET, CHIP_NONE, "a cause, logged before the restart decision: the retry or 76-* after it shows it" },
    { "pid_file-no-cgroup",  K_CHIP,  CHIP_CUT, "" },
    { "promote",             K_READY, CHIP_NONE, "" },
    { "readiness-lost",      K_QUIET, CHIP_NONE, "a cause: the retry or 76-* excise that follows shows it as the reason" },
    { "reclaim",             K_QUIET, CHIP_NONE, "pressure handling, shown on schema-board" },
    { "reload-cmd",          K_QUIET, CHIP_NONE, "reload-done or reload-fail follows" },
    { "reload-done",         K_QUIET, CHIP_NONE, "a clean reload" },
    { "reload-fail",         K_CHIP,  CHIP_FAILED, "" },
    { "reload-signal",       K_QUIET, CHIP_NONE, "a reload signal sent" },
    { "reload-timeout",      K_CHIP,  CHIP_FAILED, "" },
    { "restart-hold",        K_QUIET, CHIP_NONE, "the retry line just before it is shown" },
    { "retry",               K_CHIP,  CHIP_RETRY, "" },
    { "retry-deep",          K_CHIP,  CHIP_RETRY, "" },
    { "socket-idle",         K_QUIET, CHIP_NONE, "idle exit by design; PID 1 keeps the socket" },
    { "socket-wait",         K_CHIP,  CHIP_WAITING, "" },
    { "socket-wake",         K_QUIET, CHIP_NONE, "its spawn line follows" },
    { "spawn",               K_SPAWN, CHIP_NONE, "" },
    { "spawn-fail",          K_CHIP,  CHIP_CUT, "" },
    { "standby",             K_QUIET, CHIP_NONE, "stopped by design; counted as WAITING" },
    { "start-timeout",       K_QUIET, CHIP_NONE, "a cause: the retry or 76-excised that follows shows it as the reason" },
    { "stop-cmd",            K_QUIET, CHIP_NONE, "stop-cmd-done or a failure follows" },
    { "stop-cmd-done",       K_QUIET, CHIP_NONE, "a clean stop command" },
    { "stop-cmd-failed",     K_CHIP,  CHIP_FAILED, "" },
    { "stop-cmd-fork-fail",  K_CHIP,  CHIP_FAILED, "" },
    { "stop-cmd-timeout",    K_CHIP,  CHIP_FAILED, "" },
    { "stop-escalate",       K_CHIP,  CHIP_KILLED, "" },
    { "stop-post",           K_QUIET, CHIP_NONE, "the post-stop command started" },
    { "thaw",                K_QUIET, CHIP_NONE, "pressure handling, shown on schema-board" },
    { "thaw-sig",            K_QUIET, CHIP_NONE, "pressure handling, shown on schema-board" },
    { "timeout-recovery",    K_QUIET, CHIP_NONE, "the retry that follows is the one RETRY line for this start timeout" },
    { "timer-catchup",       K_CHIP,  CHIP_TIMER, "" },
    { "timer-done",          K_QUIET, CHIP_NONE, "the timer run finished cleanly" },
    { "timer-failed",        K_CHIP,  CHIP_FAILED, "" },
    { "timer-fire",          K_CHIP,  CHIP_TIMER, "" },
    { "wait-socket",         K_CHIP,  CHIP_WAITING, "" },
    { "watchdog-abort",      K_CHIP,  CHIP_CUT, "" },
    { "watchdog-kill",       K_CHIP,  CHIP_CUT, "" },
};

static int ev_find(const char *event) {
    size_t w = strcspn(event, " ");
    for (size_t i = 0; i < sizeof EV / sizeof *EV; i++) {
        size_t k = strlen(EV[i].key);
        if (EV[i].key[k - 1] == '*') {
            if (!strncmp(event, EV[i].key, k - 1) && w > k - 1) {
                /* a longer exact key ("card-done") wins over "card-*" */
                for (size_t j = 0; j < sizeof EV / sizeof *EV; j++)
                    if (strlen(EV[j].key) == w && !strncmp(EV[j].key, event, w)) return (int)j;
                return (int)i;
            }
        } else if (k == w && !strncmp(event, EV[i].key, w))
            return (int)i;
    }
    return -1;
}

int rail_event_known(const char *event) { return ev_find(event) >= 0; }

const char *rail_event_why(const char *event) {
    int i = ev_find(event);
    return i < 0 ? "" : EV[i].why;
}

/* FULL_TRUST is not ready: the engine sets it at spawn and leaves it on readiness */
static int is_ready_state(uint8_t st) {
    return st == STATE_FUNDAMENTAL || st == STATE_SETTLED || st == STATE_PERFECT;
}

int rail_is_starting(uint8_t state, int pid, int held_on_socket) {
    return pid > 0 && (state == STATE_FULL_TRUST || state == STATE_NEW_PROCESS) && !held_on_socket;
}

rail_chip_t rail_map_event(const char *event, const rail_svc_t *s) {
    int i = ev_find(event);
    if (i < 0) return CHIP_NONE;
    switch (EV[i].kind) {
    case K_CHIP:  return EV[i].chip;
    case K_DIED:  return s->restart_pending ? CHIP_NONE : CHIP_CUT;
    case K_END:   return s->failed ? CHIP_CUT : CHIP_EXITED;
    case K_READY:
        /* readiness that failed its probe lands in EXCISED and still logs promote */
        if (s->failed) return CHIP_CUT;
        return s->state == STATE_PERFECT ? CHIP_PERFECT : is_ready_state(s->state) ? CHIP_ONLINE : CHIP_NONE;
    case K_SPAWN: return CHIP_SPAWN;
    default:      return CHIP_NONE;
    }
}

void rail_counts(const rail_svc_t *v, int n, rail_counts_t *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < n; i++) {
        if (v[i].starting) c->starting++;
        else if (v[i].failed) c->cut++;
        else if (is_ready_state(v[i].state)) c->online++;
        else if (v[i].state == STATE_FRICTION || v[i].state == STATE_RECOVERY || v[i].state == STATE_DORMANT) c->friction++;
        else c->waiting++;
    }
}

#include "service.h"

static struct {
    const rail_io_t *io;
    int font, armed, finished, shutdown, runtime_only;
    int64_t start, last_event;
    struct { int64_t spawn_at; char spawn_pending, online, perfect; char why[48]; } s[MAX_SERVICES];
} R;

void rail_init(const rail_io_t *io, int fontmode, int reexeced, int64_t pid1_start_ms) {
    memset(&R, 0, sizeof R);
    R.io = io;
    R.font = fontmode;
    R.runtime_only = reexeced;
    R.start = R.last_event = pid1_start_ms;
}

static void emit(const char *buf, size_t n) {
    if (R.io && n) R.io->write(buf, n);
}

void rail_banner(const char *version, const char *host, int nsvc) {
    char b[512];
    if (R.runtime_only) return;
    emit(b, rail_fmt_banner(b, sizeof b, "INIT", version, host, nsvc, R.font));
}

static void show(const char *name, rail_chip_t chip, int n, const char *reason) {
    char b[1024];
    int rt = R.runtime_only || R.finished || R.shutdown;
    rail_line_t l = { .name = name, .chip = chip, .retry_n = n, .runtime = rt };
    snprintf(l.reason, sizeof l.reason, "%s", reason ? reason : "");
    l.t_ms = rt ? (int64_t)R.io->wall() * 1000 : R.io->now_ms() - R.start;
    emit(b, rail_fmt_line(b, sizeof b, &l, R.font));
}

void rail_event(const rail_svc_t *s, int idx, const char *event) {
    rail_chip_t chip = rail_map_event(event, s);
    int i = ev_find(event);
    int ok = idx >= 0 && idx < MAX_SERVICES;

    R.last_event = R.io ? R.io->now_ms() : 0;
    if (i < 0 || !ok) return;
    /* the cause of a death, for the RETRY line that follows it */
    /* a specific cause (signal, lost readiness) beats the generic died that follows it */
    if (!strncmp(event, "killed-SIG", 10) || !strcmp(event, "readiness-lost") || !strcmp(event, "start-timeout") ||
        !strcmp(event, "pid_file-missing"))
        snprintf(R.s[idx].why, sizeof R.s[idx].why, "%.*s", (int)strcspn(event, " "), event);
    else if (EV[i].kind == K_DIED && !R.s[idx].why[0])
        snprintf(R.s[idx].why, sizeof R.s[idx].why, "%s", event);
    if (EV[i].kind == K_SPAWN) {
        R.s[idx].spawn_at = R.last_event;
        R.s[idx].spawn_pending = 1;
        R.s[idx].online = R.s[idx].perfect = 0;
        R.s[idx].why[0] = '\0';   /* a cause from before this start is not this one's */
        return;
    }
    if (chip == CHIP_NONE) return;
    if (EV[i].kind == K_READY && chip == CHIP_CUT)
        snprintf(R.s[idx].why, sizeof R.s[idx].why, "probe-failed");
    else if (EV[i].kind == K_READY) {
        if (chip == CHIP_ONLINE && R.s[idx].online) return;
        if (chip == CHIP_PERFECT && R.s[idx].perfect) return;
        R.s[idx].online = 1;
        if (chip == CHIP_PERFECT) R.s[idx].perfect = 1;
    }
    /* an outcome before the 2 s mark: the SPAWN line would be stale */
    R.s[idx].spawn_pending = 0;
    if (chip == CHIP_RETRY) {
        show(s->name, chip, s->restart_count, R.s[idx].why[0] ? R.s[idx].why : event);
        R.s[idx].why[0] = '\0';
        return;
    }
    show(s->name, chip, 0, chip == CHIP_CUT || chip == CHIP_FAILED ? (R.s[idx].why[0] ? R.s[idx].why : event) : "");
    if (chip == CHIP_CUT) R.s[idx].why[0] = '\0';
}

void rail_arm_settle(void) { R.armed = 1; }
void rail_shutdown_begin(void) { R.shutdown = 1; }

int64_t rail_tick(const rail_svc_t *v, int n, int64_t now_ms) {
    int64_t next = INT64_MAX;
    int starting = 0;
    char b[512];

    for (int i = 0; i < n && i < MAX_SERVICES; i++) {
        if (R.s[i].spawn_pending) {
            if (!v[i].starting) R.s[i].spawn_pending = 0;
            else if (now_ms - R.s[i].spawn_at >= 2000) {
                R.s[i].spawn_pending = 0;
                show(v[i].name, CHIP_SPAWN, 0, "");
            } else if (R.s[i].spawn_at + 2000 < next)
                next = R.s[i].spawn_at + 2000;
        }
        starting += v[i].starting;
    }
    if (R.armed && !R.finished && !R.shutdown && !R.runtime_only) {
        int timed_out = now_ms - R.start >= 120000;
        /* a dependent spawns a tick after its dependency is ready: quiet for 1 s first */
        int quiet = now_ms - R.last_event >= 1000;
        if ((!starting && quiet) || timed_out) {
            rail_counts_t c;
            rail_counts(v, n, &c);
            R.finished = 1;
            emit(b, rail_fmt_finish(b, sizeof b, &c, timed_out && starting ? 120000 : R.last_event - R.start,
                                    timed_out && starting, R.font));
        } else {
            if (R.start + 120000 < next) next = R.start + 120000;
            if (!starting && R.last_event + 1000 < next) next = R.last_event + 1000;
        }
    }
    return next;
}

void rail_shutdown_banner(void) {
    char b[256];
    R.shutdown = 1;
    emit(b, rail_fmt_banner(b, sizeof b, "HALT", NULL, NULL, 0, R.font));
}

void rail_shutdown_outcome(const char *name, rail_chip_t chip) {
    R.shutdown = 1;
    show(name, chip, 0, "");
}

void rail_shutdown_note(const char *text) {
    char b[512];
    emit(b, rail_fmt_note(b, sizeof b, text, R.font));
}

void rail_shutdown_end(int64_t ms, int reboot) {
    char b[256];
    emit(b, rail_fmt_shutdown_end(b, sizeof b, ms, reboot, R.font));
}
