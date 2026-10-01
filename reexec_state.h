#ifndef REEXEC_STATE_H
#define REEXEC_STATE_H

#include <string.h>
#include "service.h"

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
    X(INT,  ready_path_verified) \
    X(INT,  notify_ready)        \
    X(STR,  notify_status)       \
    X(INT,  ctl_killed)          \
    X(INT,  exit_status)         \
    X(INT,  term_signal)         \
    X(INT,  core_dumped)         \
    X(INT,  has_exited)          \
    X(TS,   timer_next)          \
    X(TS,   spawn_time_mono)     \
    X(STR,  cgroup_path)         \
    X(INT,  is_frozen)

static inline void svc_runtime_copy(service_t *dst, const service_t *src) {
#define X(kind, f) memcpy(&dst->f, &src->f, sizeof dst->f);
    SVC_RUNTIME_FIELDS(X)
#undef X
}

/* A run-once timer (on_boot_sec, no interval) clears SVC_TIMER when it
 * completes. A fresh parse has the flag back; carry the terminal fact or the
 * expired timer_next fires it again. live_done: the live table had cleared it. */
static inline void svc_carry_timer_done(service_t *fresh, int live_done) {
    if (live_done && (fresh->flags & SVC_TIMER) &&
        !(fresh->flags & SVC_TIMER_CALENDAR) && fresh->timer_interval_sec <= 0)
        fresh->flags &= ~SVC_TIMER;
}

#endif
