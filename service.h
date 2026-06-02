#ifndef SERVICE_H
#define SERVICE_H

#include <sys/types.h>
#include <time.h>
#include "schema.h"

#define MAX_SERVICES    64
#define MAX_ARGV        16
#define MAX_DEPS        8
#define MAX_RESTARTS    5
#define COOLDOWN_SECS   5
#define STABLE_SECS     10   /* seconds running before FULL_TRUST -> FUNDAMENTAL */
#define MEM_MIN_KB      8192 /* minimum free memory to attempt spawn */

#define SVC_ONESHOT     (1 << 0)  /* 88 on clean exit, don't restart      */
#define SVC_NEEDS_ROOT  (1 << 1)  /* F8_PERM_AUTH requires uid 0          */
#define SVC_CRITICAL    (1 << 2)  /* EXCISED here = system friction        */
#define SVC_NO_RESTART  (1 << 3)  /* 76 on any death, no recovery arc     */

typedef enum {
    PRIO_PERIPHERAL = 0,
    PRIO_STANDARD,
    PRIO_CRITICAL
} prio_t;

typedef struct {
    char             name[64];
    char             exec[256];
    char            *argv[MAX_ARGV];
    char             dep_name[MAX_DEPS][64]; /* dep names as written in .svc    */
    int              dep_idx[MAX_DEPS];      /* resolved service indices, -1=none */
    int              grp_dep_idx[MAX_DEPS];  /* resolved group indices, -1=none */
    int              flags;                  /* SVC_* flags above               */

    schema_instance_t inst;
    pid_t            child_pid;
    int              restart_count;
    time_t           last_start;
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
    int              is_frozen;        /* status tracker for frozen services */
    struct timespec  stable_time;      /* CLOCK_MONOTONIC when FUNDAMENTAL/PERFECT reached */
    int              exit_status;
    char             cgroup_path[128]; /* /sys/fs/cgroup/schema-init/<name>   */
    time_t           dormant_until;    /* epoch when DORMANT->NEW_PROCESS fires */
    uint8_t          dormant_count;    /* backoff multiplier: delay = min(300<<n, 3600) */
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
