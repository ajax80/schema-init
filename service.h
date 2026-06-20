#ifndef SERVICE_H
#define SERVICE_H

#include <sys/types.h>
#include <stdint.h>
#include <time.h>
#include <pwd.h>
#include "schema.h"

#define MAX_SERVICES    64
#define MAX_ARGV        16
#define MAX_DEPS        8
#define MAX_RESTARTS    5
#define COOLDOWN_SECS   5
#define STABLE_SECS     10   /* seconds running before FULL_TRUST -> FUNDAMENTAL */
#define ONESHOT_START_TIMEOUT 90 /* default kill window for a oneshot stuck in FULL_TRUST */
#define MEM_MIN_KB      8192 /* minimum free memory to attempt spawn */

#define SVC_ONESHOT     (1 << 0)  /* 88 on clean exit, don't restart      */
#define SVC_NEEDS_ROOT  (1 << 1)  /* F8_PERM_AUTH requires uid 0          */
#define SVC_CRITICAL    (1 << 2)  /* EXCISED here = system friction        */
#define SVC_NO_RESTART  (1 << 3)  /* 76 on any death, no recovery arc     */
#define SVC_TIMER       (1 << 4)  /* periodic: re-arm to NEW_PROCESS on clock */
#define SVC_TIMER_CALENDAR (1 << 5) /* timer_next is a CLOCK_REALTIME wall-clock target */

typedef enum {
    PRIO_PERIPHERAL = 0,
    PRIO_STANDARD,
    PRIO_CRITICAL
} prio_t;

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
    char             cgroup_path[128]; /* /sys/fs/cgroup/schema-init/<name>   */
    struct timespec  dormant_until;    /* CLOCK_MONOTONIC when DORMANT->NEW_PROCESS fires */
    uint8_t          dormant_count;    /* backoff multiplier: delay = min(300<<n, 3600) */
    int              start_timeout_sec;  /* kill if not promoted by spawn+N; -1=unset, 0=off */
    struct timespec  spawn_time_mono;    /* CLOCK_MONOTONIC when spawned                    */
    int              timer_boot_sec;     /* on_boot_sec: delay from boot to first fire     */
    int              timer_interval_sec; /* on_active_sec: gap after each completion        */
    int              timer_cal_hour;     /* on_calendar=HH:MM hour; -1 = not a calendar timer */
    int              timer_cal_min;      /* on_calendar=HH:MM minute                          */
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

#endif
