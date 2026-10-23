#ifndef ENGINE_H
#define ENGINE_H

#include <sys/types.h>
#include "service.h"
#include "group.h"
#include "schema_shm.h"
#include "reexec_state.h"
#include <stdint.h>
#include <time.h>

#define CTL_SOCK_PATH   "/run/schema-init.sock"
#define TIMER_STAMP_DIR "/var/lib/schema-init/timers"

typedef struct {
    const char *ctl_sock;
    const char *run_dir;
    const char *notify_sock;
    const char *shm_name;
    const char *timer_stamp_dir;
    const char *svc_dir;
    const char *ctl_fallback;   /* NULL = no fallback */
    const char *env_fallback;
    const char *svc_fallback;
    const char *tag;            /* log line prefix */
} engine_paths_t;

extern engine_paths_t engine_paths;

int engine_mkdir_p(const char *path, mode_t mode);

/* 1 = reload refuses a .svc changed since boot (PID 1: config edits wait for
 * the next boot). A user supervisor's files are its user's to edit. */
extern int engine_reload_integrity;
/* 1 = a reload to zero services is adopted. PID 1 refuses it (an empty
 * config there is a broken box); a user with nothing enabled is normal. */
extern int engine_reload_allow_empty;

typedef struct {
    void (*loop_pressure)(int psi_fired);
    /* absolute monotonic-ms deadline of the next host wake, or WAKE_NONE;
     * sets *tick for a source that needs the fixed tick */
    int64_t (*wake_due)(int64_t now_m, int *tick);
    void (*loop_tail)(void);
    int  (*ctl_extra)(int fd, char *line);
    void (*shut_log)(const char *msg);
    void (*on_sigcont)(void);
    int  (*on_reaped)(pid_t pid, int status);
    void (*after_sockets)(void);
} engine_hooks_t;

extern engine_hooks_t engine_hooks;

void engine_hook_noop(void);
void engine_hook_pressure_none(int psi_fired);
int64_t engine_hook_wake_none(int64_t now_m, int *tick);
int  engine_hook_ctl_none(int fd, char *line);
void engine_hook_log_none(const char *msg);
int  engine_hook_reaped_none(pid_t pid, int status);

extern service_t   *services;
extern service_t   *shadow_services;
extern int          svc_count;
extern group_t     *shadow_groups;
extern eviction_t evictions[MAX_EVICTIONS];
extern int        eviction_count;
void eviction_readded(void);
extern volatile int engine_running;
extern volatile int engine_do_reboot;
extern schema_shm_t *shm_ptr;
extern int          ctl_fd;
extern int          notify_fd;
extern int          watchdog_fd;
extern int64_t      watchdog_pet_ms;
extern time_t       notify_paused_since;
extern int          system_under_pressure;
extern int          psi_fd;
extern int64_t      cpu_check_due;
extern struct timespec init_start;
extern pid_t   reexec_check_pid;
extern int64_t reexec_check_due;
extern int     reexec_check_killed;
time_t mono_sec(void);
uint64_t monotonic_ms(void);
void ctl_writef(int fd, const char *fmt, ...);
void set_nonblock(int fd);
void notify_pause_track(void);
int validate_and_resolve(service_t *svc_table, int s_count, group_t *grp_table, int g_count);
int engine_load(const char *svc_dir);
int engine_reload(int evict, char *err, size_t errsz);

#define SHUT_GRACE_MS   3000
void engine_open(int adopting);
void engine_arm_timers(int adopting);
void engine_run(void);
void engine_stop_services(void);
void engine_kill_services(void);
void engine_release(void);
void engine_free(void);

#endif
