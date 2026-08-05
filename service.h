#ifndef SERVICE_H
#define SERVICE_H

#include <sys/types.h>
#include <stdint.h>
#include <time.h>
#include <pwd.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "schema.h"

#define MAX_SERVICES    64
#define MAX_ARGV        16
#define MAX_DEPS        8
#define MAX_RESTARTS    5
#define COOLDOWN_SECS   5
#define STABLE_SECS     10   /* seconds running before FULL_TRUST -> FUNDAMENTAL */
#define ONESHOT_START_TIMEOUT 90 /* default kill window for a oneshot stuck in FULL_TRUST */
#define MEM_MIN_KB      8192 /* minimum free memory to attempt spawn */

#define RECLAIM_FLOOR   (4L * 1024 * 1024)   /* skip cgroups holding < 4 MiB */
#define RECLAIM_CAP     (128L * 1024 * 1024) /* reclaim at most this per service */

/* how many bytes of cold memory to ask the kernel to reclaim from a cgroup
 * currently holding `current` bytes: half of it, never more than `cap`, and
 * nothing at all below the floor. Pure — unit-tested in tests/test_reclaim.c. */
static inline long reclaim_target(long current, long cap) {
    long half;
    if (current < RECLAIM_FLOOR) return 0;
    half = current / 2;
    return half < cap ? half : cap;
}

#define SVC_ONESHOT     (1 << 0)  /* 88 on clean exit, don't restart      */
#define SVC_NEEDS_ROOT  (1 << 1)  /* F8_PERM_AUTH requires uid 0          */
#define SVC_CRITICAL    (1 << 2)  /* EXCISED here = system friction        */
#define SVC_NO_RESTART  (1 << 3)  /* 76 on any death, no recovery arc     */
#define SVC_TIMER       (1 << 4)  /* periodic: re-arm to NEW_PROCESS on clock */
#define SVC_TIMER_CALENDAR (1 << 5) /* timer_next is a CLOCK_REALTIME wall-clock target */
#define SVC_TIMER_PERSIST  (1 << 6) /* persistent=1: catch up a calendar fire missed while down */
#define SVC_NO_NEW_PRIVS   (1 << 7)  /* prctl(PR_SET_NO_NEW_PRIVS) in child   */

typedef enum {
    PRIO_PERIPHERAL = 0,
    PRIO_STANDARD,
    PRIO_CRITICAL
} prio_t;

/* cgroup v2 resource tiering derived from a service's priority class. Pure —
 * unit-tested in tests/test_cgroup_tiering.c. cpu_weight is 0 for PERIPHERAL
 * because cpu.idle=1 and cpu.weight are mutually exclusive: when cpu_idle is
 * set the caller writes cpu.idle and skips cpu.weight. */
typedef struct {
    int cpu_weight; /* 100 STANDARD, 1000 CRITICAL; 0 when cpu_idle set */
    int io_weight;  /* 10 PERIPHERAL, 100 STANDARD, 1000 CRITICAL */
    int cpu_idle;   /* 1 PERIPHERAL, 0 otherwise */
} cgroup_tier_t;

static inline cgroup_tier_t cgroup_tiering(prio_t priority) {
    cgroup_tier_t tier = {0, 0, 0};
    switch (priority) {
    case PRIO_PERIPHERAL:
        tier.cpu_idle = 1; tier.cpu_weight = 0;    tier.io_weight = 10;   break;
    case PRIO_CRITICAL:
        tier.cpu_idle = 0; tier.cpu_weight = 1000; tier.io_weight = 1000; break;
    case PRIO_STANDARD:
    default:
        tier.cpu_idle = 0; tier.cpu_weight = 100;  tier.io_weight = 100;  break;
    }
    return tier;
}

/* soft reclaim buffer (memory.high): 90% of the hard cap, in bytes; 0 (no
 * memory.high) when the service has no mem_limit. Pure. */
static inline long mem_high_bytes(long mem_limit_mb) {
    if (mem_limit_mb <= 0) return 0;
    return (mem_limit_mb * 1024L * 1024L * 9L) / 10L;
}

/* on_calendar day-of-week token -> tm_wday index (Sun=0..Sat=6), or -1 if the
 * token is not one of the canonical 3-letter names (case-insensitive). Pure. */
static inline int calendar_dow_from_name(const char *s) {
    static const char *const n[7] = {"sun","mon","tue","wed","thu","fri","sat"};
    char b[4] = {0};
    int i;
    for (i = 0; i < 3 && s[i]; i++) b[i] = (char)tolower((unsigned char)s[i]);
    if (s[i] != '\0') return -1;               /* longer than 3 chars -> reject */
    for (i = 0; i < 7; i++) if (strcmp(b, n[i]) == 0) return i;
    return -1;
}

/* Parse an on_calendar value into fields. An optional leading token selects the
 * recurrence: none = daily, a 3-letter weekday = weekly, a 1..31 number =
 * monthly on that day-of-month.
 *   "HH:MM"       -> dow=-1 dom=-1   (every day)
 *   "Mon HH:MM"   -> dow=0..6 dom=-1 (that weekday)
 *   "15 HH:MM"    -> dow=-1  dom=1..31 (that day of month)
 * Returns 0 on success, -1 on any malformed input (a typo must not silently
 * schedule garbage). Pure — no clock, unit-tested in tests/test_calendar.c. */
static inline int parse_calendar_fields(const char *val, int *hour, int *min,
                                        int *dow, int *dom) {
    int h = -1, m = -1, dw = -1, dm = -1;
    char tok[16], extra;
    if (sscanf(val, "%15s %d:%d %c", tok, &h, &m, &extra) == 3) {
        if (isalpha((unsigned char)tok[0])) {
            dw = calendar_dow_from_name(tok);
            if (dw < 0) return -1;
        } else if (isdigit((unsigned char)tok[0])) {
            char *end;
            long v = strtol(tok, &end, 10);
            if (*end != '\0' || v < 1 || v > 31) return -1;
            dm = (int)v;
        } else {
            return -1;
        }
    } else if (sscanf(val, "%d:%d %c", &h, &m, &extra) != 2) {
        return -1;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    *hour = h; *min = m; *dow = dw; *dom = dm;
    return 0;
}

/* Does a wall-clock day satisfy a calendar timer's day constraints? dow/dom of
 * -1 mean "unconstrained". wday/mday come from a struct tm. Pure. */
static inline int calendar_day_matches(int dow, int dom, int wday, int mday) {
    if (dow >= 0 && wday != dow) return 0;
    if (dom >= 1 && mday != dom) return 0;
    return 1;
}

/* Earliest local-time instant strictly after `after` whose HH:MM and day match
 * the constraints. Walks forward day-by-day; mktime re-normalizes wday/mday and
 * self-corrects DST each step. Daily (dow=dom=-1) resolves on the first/second
 * day. Deterministic under a fixed TZ — unit-tested in tests/test_calendar.c. */
static inline time_t calendar_next_after(int hour, int min, int dow, int dom,
                                         time_t after) {
    struct tm tm, ck;
    for (int d = 0; d <= 400; d++) {
        localtime_r(&after, &tm);
        tm.tm_mday += d;
        tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = 0; tm.tm_isdst = -1;
        time_t t = mktime(&tm);
        if (t == (time_t)-1) continue;
        localtime_r(&t, &ck);
        if (t > after && calendar_day_matches(dow, dom, ck.tm_wday, ck.tm_mday))
            return t;
    }
    return after + 86400;   /* unreachable fallback */
}

/* Most recent matching instant at or before `now`; 0 if none within ~400 days
 * (constraint never satisfiable). Mirror of calendar_next_after, walking back. */
static inline time_t calendar_recent_at_or_before(int hour, int min, int dow,
                                                   int dom, time_t now) {
    struct tm tm, ck;
    for (int d = 0; d <= 400; d++) {
        localtime_r(&now, &tm);
        tm.tm_mday -= d;
        tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = 0; tm.tm_isdst = -1;
        time_t t = mktime(&tm);
        if (t == (time_t)-1) continue;
        localtime_r(&t, &ck);
        if (t <= now && calendar_day_matches(dow, dom, ck.tm_wday, ck.tm_mday))
            return t;
    }
    return 0;
}

enum {
    PART_MEMBER = 0,
    PART_ROOT,
    PART_ISOLATED
};

typedef struct {
    char             name[64];
    char             instance[32];    /* template instance ID, e.g. "12" from motor@12.svc */
    char             exec[256];
    char            *argv[MAX_ARGV];
    char             dep_name[MAX_DEPS][64]; /* dep names as written in .svc    */
    int              dep_idx[MAX_DEPS];      /* resolved service indices, -1=none */
    int              grp_dep_idx[MAX_DEPS];  /* resolved group indices, -1=none */
    int              flags;                  /* SVC_* flags above               */

    schema_instance_t inst;
    pid_t            child_pid;
    int              restart_count;
    int              max_restarts;
    time_t           last_start;
    uid_t            run_uid;            /* if non-zero: drop to this uid before exec */
    gid_t            run_gid;            /* companion gid for run_uid                 */
    char             run_user[32];       /* username string for initgroups            */



    time_t           start_time;       /* when current run began              */
    int              stable_secs;      /* seconds until FULL_TRUST->FUNDAMENTAL; default STABLE_SECS */
    char             ready_path[256];  /* if set, promote when this path exists (fallback: stable_secs) */
    prio_t           priority;         /* priority class for resource throttling */
    int              fuse;             /* 1 to enable quarantine cascade */
    char             fuse_cmd[256];    /* shell command executed on fuse trip */
    char             failsafe_cmd[256]; /* shell command executed on failsafe */
    int              failsafe_timeout_ms; /* timeout for failsafe execution */
    int              ready_poll_hz;    /* polling rate for ready_path check at runtime */
    int              ready_check_ticks; /* tick counter for ready_path polling */
    int              ready_path_verified; /* 1 if ready_path has been verified at least once */
    pid_t            failsafe_pid;     /* PID of running failsafe command, 0 if none */
    struct timespec  failsafe_start;   /* CLOCK_MONOTONIC when failsafe execution began */
    int              no_excise;        /* 1 to prevent transition to STATE_EXCISED */
    int              watchdog_timeout_ms; /* service watchdog window (0 = disabled) */
    struct timespec  last_pet;            /* CLOCK_MONOTONIC timestamp of last pet */
    int              is_frozen;        /* status tracker for frozen services */
    struct timespec  stable_time;      /* CLOCK_MONOTONIC when FUNDAMENTAL/PERFECT reached */
    int              exit_status;
    int              cpu_limit_pct;    /* 1-100: % of one CPU core via cpu.max; 0 = unlimited */
    long             mem_limit_mb;     /* MB hard cap via memory.max; 0 = unlimited */
    char             cpuset[64];       /* CPU affinity list for cpuset.cpus (e.g. "2,3"); empty = unconstrained */
    int              cpuset_partition; /* PART_MEMBER/ROOT/ISOLATED; cgroupv2 cpuset.cpus.partition */
    int              allowed_slot_min; /* slot boundary gate: -1 = unconstrained */
    int              allowed_slot_max; /* slot boundary gate: -1 = unconstrained */
    uint32_t         content_hash;     /* FNV-1a hash of the parsed .svc file at load time */
    uint64_t         cap_keep_mask;    /* keep_caps allowlist; bit N = CAP_N       */
    uint8_t          cap_restrict;     /* 1 if keep_caps= was present in the .svc  */
    char             cgroup_path[128]; /* /sys/fs/cgroup/schema-init/<name>   */
    struct timespec  dormant_until;    /* CLOCK_MONOTONIC when DORMANT->NEW_PROCESS fires */
    uint8_t          dormant_count;    /* backoff multiplier: delay = min(300<<n, 3600) */
    int              start_timeout_sec;  /* kill if not promoted by spawn+N; -1=unset, 0=off */
    struct timespec  spawn_time_mono;    /* CLOCK_MONOTONIC when spawned                    */
    int              timer_boot_sec;     /* on_boot_sec: delay from boot to first fire     */
    int              timer_interval_sec; /* on_active_sec: gap after each completion        */
    int              timer_cal_hour;     /* on_calendar=HH:MM hour; -1 = not a calendar timer */
    int              timer_cal_min;      /* on_calendar=HH:MM minute                          */
    int              timer_cal_dow;      /* on_calendar weekday 0=Sun..6=Sat; -1 = any day    */
    int              timer_cal_dom;      /* on_calendar day-of-month 1..31; -1 = any          */
    struct timespec  timer_next;         /* next-fire deadline: CLOCK_MONOTONIC, or CLOCK_REALTIME if SVC_TIMER_CALENDAR */
} service_t;

/* build the F8 flag word by inspecting the real system */
uint32_t service_probe_f8(service_t *svc, service_t *table, int count);

/* build F9 flag word after a service death */
uint32_t service_probe_f9(service_t *svc, service_t *table, int count);

/* build F6 flag word after recovery fails */
uint32_t service_probe_f6(service_t *svc);

/* fork+exec the service; sets child_pid and last_start */
int service_spawn(service_t *svc);

/* apply opt-in hardening in the child, before setuid/execv; -1 -> fail closed */
int service_apply_hardening(const service_t *svc);

/* log one line about the service's current schema state */
void service_log(const service_t *svc, const char *event);

/* kill all processes in the service's cgroup and remove it */
void service_cgroup_kill(service_t *svc);

/* 1 if all deps (service and group) are stable, 0 otherwise */
int service_deps_ready(service_t *svc, service_t *stable, int scount,
                       const uint8_t *grp_states, int gcount);

/* parse a simple service file; returns number loaded */
int services_load(const char *dir, service_t *table, int max);

/* parse a single .svc file into svc; returns 0 on success, -1 on failure */
int service_load_one(const char *path, service_t *svc);

/* detect dependency cycles; logs and returns number of cycles found */
int services_check_cycles(service_t *table, int count);

/* call between fork() and exec() in every child: clear the inherited mask */
void service_reset_child_sigmask(void);

/* call once, early in PID 1: raise our own soft RLIMIT_NOFILE to the hard one */
void service_raise_pid1_nofile(void);

/* call between fork() and exec(): give the child back PID 1's original soft
 * NOFILE, so nothing inherits a raised limit that breaks select() */
void service_restore_child_nofile(void);

#endif
