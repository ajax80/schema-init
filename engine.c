#define FDCLOSE_DEFINE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <termios.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#include "schema.h"
#include "engine.h"
#include "service.h"
#include "svc_serial.h"
#include "svc_dropins.h"
#include "reexec_state.h"
#include "dynuser.h"
#include "notify.h"
#include "fdstore.h"
#include "coredump.h"
#include <sys/resource.h>
#include "group.h"
#include "schema_shm.h"
#include "recorder.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdarg.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <glob.h>
#include <sys/ioctl.h>
#include <grp.h>
#include <sys/inotify.h>
#include <sys/random.h>
#include <linux/watchdog.h>
#include <dirent.h>
#include <mntent.h>

static void elog(FILE *f, const char *fmt, ...) {
    va_list ap;
    fprintf(f, "[%s] ", engine_paths.tag);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
}

int engine_mkdir_p(const char *path, mode_t mode) {
    char buf[PATH_MAX];
    if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf) { errno = ENAMETOOLONG; return -1; }
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, mode) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

#define TICK_USEC       250000   /* 250ms main loop tick */
#define READY_POLL_MS   10       /* while a starting service's ready_path is awaited, */
#define READY_POLL_SECS 3        /* ...for this long after its spawn */
#define PID_FILE_WAIT_SECS 5
#define MAINPID_WAIT_SECS 10

static service_t    services_a[MAX_SERVICES];
static service_t    services_b[MAX_SERVICES];
service_t   *services = services_a;
service_t   *shadow_services = services_b;
int          svc_count = 0;

static group_t      groups_a[MAX_GROUPS];
static group_t      groups_b[MAX_GROUPS];
static group_t     *groups = groups_a;
group_t     *shadow_groups = groups_b;
static int          grp_count = 0;
#define EVICT_GRACE_SECS  3
/* a binder's own exit this close before its bound goes down was the bound's doing */
#define BIND_GRACE_MS     2000
#define EVICT_KILL_WAIT_SECS 5
#define EVICT_STUCK_RECHECK_SECS 60

eviction_t evictions[MAX_EVICTIONS];
int        eviction_count = 0;

volatile int engine_running = 1;
volatile int engine_do_reboot = 0;
schema_shm_t *shm_ptr = NULL;
int          ctl_fd   = -1;
int          notify_fd = -1;
static int          sig_fd   = -1;
int          watchdog_fd = -1;
int64_t      watchdog_pet_ms = 5000;
time_t       notify_paused_since = 0;   /* mono sec, 0 = notify reads flowing */
static int          bus_reporter_seen = 0;  /* a broker BUSNAME= report was accepted: ready_bus_name waits for its name */
int          system_under_pressure = 0;
static int          pass_changed = 1;   /* first pass, or last pass moved a service: run at once */
#define PASS_RERUN_MAX  16                /* ...at most this many passes in a row */
int          psi_fd = -1;
static int          ino_fd = -1;
/* watch_path= has its own fd: a ready_path dir watch keeps its narrow mask
 * instead of sharing a wd (and the widest mask) with a path watch */
static int          path_fd = -1;
static int          path_gc_due;
static int64_t      ready_event_due = 0;    /* CLOCK_MONOTONIC ms, 0 = none */
static int64_t      ready_backstop_due = 0;
int64_t      cpu_check_due = 0;
struct timespec init_start;
char booted_version[64] = SCHEMA_INIT_VERSION;
char booted_image[64] = "";

static void start_failsafe(service_t *svc);
static void active_kill_service(service_t *svc);
static void stop_main(service_t *svc);
static void svc_cgroup_kill_tree(service_t *svc);
static int svc_signal_all(service_t *svc, int sig);
static int handle_reload(int evict_mode, char *err, size_t errsz);
pid_t   reexec_check_pid = 0;
int64_t reexec_check_due = 0;
int     reexec_check_killed = 0;

/* Arm a calendar timer: set timer_next to the next CLOCK_REALTIME instant at
 * which local wall-clock reaches one of its HH:MM slots on a day matching the
 * timer's dow/dom constraints, strictly in the future. Walks forward
 * day-by-day (mktime per step re-normalizes wday/mday and self-corrects
 * DST/NTP each cycle). Daily timers (dow=dom=-1) match the first or second
 * day and behave as before. */
/* randomized_delay_sec: 0..N whole seconds onto the deadline just armed, a
 * fresh draw each arm. Never blocks: PID 1 can arm before the pool is ready. */
static void timer_jitter(service_t *svc) {
    uint32_t r;
    if (svc->timer_jitter_sec <= 0) return;
    if (getrandom(&r, sizeof r, GRND_NONBLOCK) != (ssize_t)sizeof r) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        r = (uint32_t)ts.tv_nsec;
        for (const char *p = svc->name; *p; p++) r = (r ^ (uint8_t)*p) * 16777619u;
    }
    svc->timer_next.tv_sec += (time_t)(r % ((uint32_t)svc->timer_jitter_sec + 1));
}

static void timer_arm_calendar(service_t *svc) {
    svc->timer_next.tv_sec  = calendar_next_after(svc->timer_cal, svc->timer_cal_n, time(NULL));
    svc->timer_next.tv_nsec = 0;
    timer_jitter(svc);
}

/* ── persistent calendar timers (persistent=1): catch up a fire missed while
 * the machine was down. Last run is stamped under engine_paths.timer_stamp_dir. ── */

static void timer_stamp_path(const service_t *svc, char *buf, size_t n) {
    snprintf(buf, n, "%s/%s.stamp", engine_paths.timer_stamp_dir, svc->name);
}

static time_t timer_stamp_read(const service_t *svc) {
    char path[256]; timer_stamp_path(svc, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long long v = 0;
    if (fscanf(f, "%lld", &v) != 1) v = 0;
    fclose(f);
    return (time_t)v;
}

static void timer_stamp_write(const service_t *svc, time_t t) {
    engine_mkdir_p(engine_paths.timer_stamp_dir, 0755);
    char path[256]; timer_stamp_path(svc, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%lld\n", (long long)t);
    fclose(f);
}

/* most recent past (or current) occurrence of any of the timer's HH:MM slots
 * on a day matching its dow/dom constraints, <= now. Walks backward
 * day-by-day; returns 0 if none in the last ~400 days (constraint never
 * satisfiable). */
static time_t calendar_recent_occurrence(const service_t *svc, time_t now) {
    return calendar_recent_at_or_before(svc->timer_cal, svc->timer_cal_n, now);
}

/* If a scheduled fire passed while we were down, fire now; otherwise arm
 * normally. A never-stamped timer is seeded (no replay of history on first
 * deploy). */
static void timer_catchup_check(service_t *svc) {
    time_t now = time(NULL);
    time_t stamp = timer_stamp_read(svc);
    if (stamp == 0) {
        timer_stamp_write(svc, now);
        timer_arm_calendar(svc);
        return;
    }
    if (calendar_recent_occurrence(svc, now) > stamp) {
        svc->timer_next.tv_sec  = now;   /* CLOCK_REALTIME: fires on the next tick */
        svc->timer_next.tv_nsec = 0;
        timer_jitter(svc);
        service_log(svc, "timer-catchup");
    } else {
        timer_arm_calendar(svc);
    }
}

/* A separate /var is mounted by mount-fstab, after the timers are armed: a
 * stamp read before then comes from the empty shadow /var, reads as "never
 * stamped" and the catch-up is lost. So a persistent timer arms on the
 * calendar and reads its stamp once mount-fstab has made one attempt (a
 * failing one never mounts /var, and must not hold timers forever); with no
 * mount-fstab, on the next pass. */
static void timer_arm_persistent(service_t *svc) {
    svc->timer_catchup_wait = 1;
    timer_arm_calendar(svc);
}

/* born "already ran": PERFECT, first fire at the calendar or boot + on_boot_sec */
static void timer_arm(service_t *svc) {
    svc->inst.state = STATE_PERFECT;
    if (svc->flags & SVC_TIMER_CALENDAR) {
        if (svc->flags & SVC_TIMER_PERSIST) timer_arm_persistent(svc);
        else timer_arm_calendar(svc);
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &svc->timer_next);
    svc->timer_next.tv_sec += svc->timer_boot_sec;
    timer_jitter(svc);
}

static void timer_catchup_tick(void) {
    int i, any = 0;
    for (i = 0; i < svc_count && !any; i++)
        any = services[i].timer_catchup_wait;
    if (!any) return;
    for (i = 0; i < svc_count; i++) {
        if (strcmp(services[i].name, "mount-fstab") != 0) continue;
        uint8_t st = services[i].inst.state;
        if (st != STATE_PERFECT && st != STATE_EXCISED &&
            st != STATE_DORMANT && st != STATE_RECOVERY)
            return;
    }
    for (i = 0; i < svc_count; i++) {
        if (!services[i].timer_catchup_wait) continue;
        services[i].timer_catchup_wait = 0;
        /* not PERFECT: it is firing on the calendar right now, that is the run */
        if (services[i].inst.state == STATE_PERFECT)
            timer_catchup_check(&services[i]);
    }
}

time_t mono_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* cgroup.events "populated 0" once every process in the subtree is gone,
 * and a cgroup that no longer exists holds nothing. Otherwise unreadable
 * counts as populated so the kill is still attempted. */
static int cgroup_populated(const char *cg) {
    char path[320], line[64];
    FILE *f;
    int pop = 1;
    snprintf(path, sizeof path, "%s/cgroup.events", cg);
    if (!(f = fopen(path, "r"))) return errno != ENOENT;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "populated %d", &pop) == 1) break;
    fclose(f);
    return pop;
}

static void eviction_add_as(pid_t pid, const char *cgroup, int reaped, int killed, int grace) {
    if (eviction_count >= MAX_EVICTIONS) return;
    evictions[eviction_count].pid      = pid;
    evictions[eviction_count].deadline = mono_sec() + grace;
    evictions[eviction_count].reaped   = reaped;
    evictions[eviction_count].killed   = killed;
    snprintf(evictions[eviction_count].cgroup, sizeof evictions[eviction_count].cgroup, "%s", cgroup);
    eviction_count++;
}


static void eviction_add(pid_t pid, const char *cgroup) {
    eviction_add_as(pid, cgroup, 0, 0, EVICT_GRACE_SECS);
}

/* A service spawning into CG owns it again: a leftover-tree entry for it
 * would judge the new instance */
static void eviction_forget(const char *cg) {
    for (int i = 0; i < eviction_count; )
        if (evictions[i].pid == 0 && !strcmp(evictions[i].cgroup, cg))
            evictions[i] = evictions[--eviction_count];
        else
            i++;
}

/* A killed tree can't be rmdir'ed until its processes are gone, and
 * cgroup.kill is asynchronous: leave the leftover to the eviction sweep,
 * which rmdirs it once it is empty (or at the 1 s backstop). */
static void svc_cgroup_kill_tree(service_t *svc) {
    char cg[sizeof svc->cgroup_path];
    snprintf(cg, sizeof cg, "%s", svc->cgroup_path);
    service_cgroup_kill(svc);
    if (cg[0] && access(cg, F_OK) == 0) {
        eviction_forget(cg);
        /* pid 0: nothing to signal, only the tree to rmdir once empty */
        eviction_add_as(0, cg, 1, 1, 1);
    }
}

static void eviction_drop(int i) {
    if (evictions[i].cgroup[0]) cgroup_rmdir_tree(evictions[i].cgroup);
    evictions[i] = evictions[--eviction_count];
}

/* An evicted child that exits on SIGTERM must not leave a live pid in the
 * table: by the deadline it may be reused, and the log would claim a kill
 * that never happened. While its cgroup still holds descendants that
 * ignored SIGTERM the entry stays, marked reaped, to cgroup.kill them at the
 * deadline. The pid itself is kept so an older image after a re-exec, which
 * ignores reaped=, behaves as it always did instead of seeing pid 0.
 *
 * Leftovers are reparented to PID 1, so each one lands here too: the entry
 * whose cgroup has just emptied is dropped and its directory removed, with
 * no polling. The service is gone from the config, so nothing else would
 * ever rmdir it. */
static void eviction_reaped(pid_t pid) {
    int i;
    for (i = 0; i < eviction_count; i++) {
        if (evictions[i].pid != pid || evictions[i].reaped) continue;
        if (evictions[i].cgroup[0]) evictions[i].reaped = 1;
        else eviction_drop(i);
        break;
    }
    for (i = 0; i < eviction_count; ) {
        if (evictions[i].cgroup[0] && (evictions[i].reaped || evictions[i].killed) &&
            !cgroup_populated(evictions[i].cgroup))
            eviction_drop(i);
        else
            i++;
    }
}

/* A reload or re-exec that brings an evicted service's name back would spawn
 * the new instance into the old cgroup (cgroup v2 cannot rename it aside),
 * and the deadline kill would take it down with the leftovers. Kill the
 * leftovers now, before the new one starts. A running service is skipped: it
 * was re-added by an older image, which kept the entry, and is in there. */
void eviction_readded(void) {
    for (int i = 0; i < eviction_count; ) {
        int s;
        for (s = 0; s < svc_count; s++)
            if (services[s].child_pid == 0 && evictions[i].cgroup[0] &&
                strcmp(evictions[i].cgroup, service_cgroup_path_for(&services[s])) == 0)
                break;
        if (s == svc_count) { i++; continue; }
        if (cgroup_populated(evictions[i].cgroup)) {
            cgroup_kill_path(evictions[i].cgroup);
            elog(stdout, "'%s' is back in the configuration: force-killed what its evicted instance left in %s\n",
                   services[s].name, evictions[i].cgroup);
        }
        eviction_drop(i);
    }
}

static void eviction_tick(void) {
    time_t now = mono_sec();
    int i = 0;
    while (i < eviction_count) {
        if (now < evictions[i].deadline) { i++; continue; }
        int live = evictions[i].cgroup[0] ? cgroup_populated(evictions[i].cgroup) : 1;
        if (evictions[i].killed) {
            if (!live) { eviction_drop(i); continue; }
            /* a task stuck in D state outlives SIGKILL; dropping the entry
             * would leak the cgroup once it finally dies, so keep checking */
            if (evictions[i].killed == 1)
                elog(stdout, "eviction: %s still populated %d s after SIGKILL\n",
                       evictions[i].cgroup, EVICT_KILL_WAIT_SECS);
            evictions[i].killed   = 2;
            evictions[i].deadline = now + EVICT_STUCK_RECHECK_SECS;
            i++;
            continue;
        }
        if (!live) { eviction_drop(i); continue; }
        int killed = evictions[i].cgroup[0] && cgroup_kill_path(evictions[i].cgroup);
        /* no cgroup, or neither cgroup.kill nor cgroup.procs usable: fall back
         * to a direct SIGKILL. previously this whole branch was gated on
         * cgroup[0], so an orphan with no cgroup that ignored SIGTERM survived
         * eviction entirely. an unreaped pid is still our child, so it cannot
         * have been reused (bar an entry adopted from an older image, which
         * may have reaped it before the re-exec). */
        if (!killed && !evictions[i].reaped && evictions[i].pid > 0) {
            /* a stop runner leads its own group: take what it started too */
            kill(-evictions[i].pid, SIGKILL);
            kill(evictions[i].pid, SIGKILL);
            killed = 1;
        }
        if (killed && !evictions[i].reaped)
            elog(stdout, "eviction grace expired: force-killed pid=%d\n",
                   (int)evictions[i].pid);
        else if (killed)
            elog(stdout, "eviction grace expired: force-killed leftovers in %s\n",
                   evictions[i].cgroup);
        if (killed && evictions[i].cgroup[0]) {
            /* SIGKILL lands asynchronously; eviction_reaped() removes the
             * cgroup once the dead are reaped, this deadline is the backstop */
            evictions[i].killed   = 1;
            evictions[i].deadline = now + EVICT_KILL_WAIT_SECS;
            i++;
            continue;
        }
        eviction_drop(i);
    }
}

/* runtime_dir=: removed once the service is stopped for good, kept across
 * respawns and backoff; waits for exec_stop_post, which may still use it */
static void runtime_dir_tick(void) {
    for (int i = 0; i < svc_count; i++) {
        service_t *s = &services[i];
        if (!s->rt_made || s->child_pid > 0 || s->stop_pid > 0 || s->inst.state != STATE_EXCISED) continue;
        service_runtime_dirs_remove(s);
        s->rt_made = 0;
    }
}

static void service_watchdog_tick(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t now_ms = ts_ms_ceil(&now);
    for (int i = 0; i < svc_count; i++) {
        service_t *svc = &services[i];
        if (svc->wd_armed_sec <= 0 || svc->child_pid <= 0 || !svc->notify_ready || svc->ctl_killed)
            continue;
        if (svc->is_frozen || notify_paused_since) {
            svc->last_pet = now;
            if (svc->wd_abort_at.tv_sec || svc->wd_abort_at.tv_nsec)
                svc->wd_abort_at = now;
            continue;
        }
        if (now_ms < svc_wd_due(&svc->last_pet, &svc->wd_abort_at, svc->wd_armed_sec)) continue;
        if (svc->wd_abort_at.tv_sec || svc->wd_abort_at.tv_nsec) {
            service_log(svc, "watchdog-kill");
            kill(svc->child_pid, SIGKILL);
            svc->wd_abort_at = (struct timespec){0, 0};
            svc->last_pet = now;
        } else {
            elog(stdout, "watchdog: '%s' sent no WATCHDOG=1 for %ds — SIGABRT\n",
                   svc->name, svc->wd_armed_sec);
            service_log(svc, "watchdog-abort");
            svc->wd_fired = 1;
            kill(svc->child_pid, SIGABRT);
            svc->wd_abort_at = now;
        }
    }
}

static void load_env_file(void) {
    char envp[160];
    const char *path = envp;
    snprintf(envp, sizeof envp, "%s/env", engine_paths.run_dir);
    int fd = service_open_trusted(path);
    if (fd < 0 && errno != EPERM && engine_paths.env_fallback)
        fd = service_open_trusted(path = engine_paths.env_fallback);
    if (fd < 0) {
        if (errno == EPERM)
            elog(stderr, "REFUSED %s: not a regular file owned by root and "
                    "writable only by its owner — not loaded\n", path);
        return;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return; }

    char line[512];
    int r;
    while ((r = svc_read_line(f, line, sizeof(line))) != 0) {
        char *start = line;
        if (r < 0) {
            elog(stderr, "%s: line longer than %zu bytes — ignored\n",
                    path, sizeof(line) - 1);
            continue;
        }
        while (*start == ' ' || *start == '\t') start++;

        char *end = start + strlen(start) - 1;
        while (end >= start && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
            *end = '\0';
            end--;
        }

        if (*start == '\0' || *start == '#') continue;

        char *eq = strchr(start, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = start;
        char *val = eq + 1;

        char *key_end = key + strlen(key) - 1;
        while (key_end >= key && (*key_end == ' ' || *key_end == '\t')) {
            *key_end = '\0';
            key_end--;
        }
        while (*val == ' ' || *val == '\t') val++;

        setenv(key, val, 1);
    }
    fclose(f);
}

/* ── zombie reaper ──────────────────────────────────────────────────── */

static void restart_budget_refresh(service_t *svc) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - svc->spawn_time_mono.tv_sec >= RESTART_RESET_SECS) {
        svc->restart_count = 0;
        svc->card_next = 0;
        svc->fallback_live = 0;
    }
}

static void breaker_clear(service_t *svc) {
    /* a cleared breaker restarts the backoff too, or the first FRICTION re-trips it */
    if (svc->circuit_broken) svc->dormant_count = 0;
    svc->circuit_broken = 0;
}

static void fallback_up(service_t *fb) {
    fb->standby_up = 1;
    fb->flags &= ~SVC_NO_RESTART;
    if (fb->child_pid <= 0 && (fb->inst.state == STATE_EXCISED || fb->inst.state == STATE_PERFECT)) {
        fb->inst.state = STATE_NEW_PROCESS;
        fb->restart_count = 0;
        breaker_clear(fb);
    }
    service_log(fb, "fallback-up");
}

static void fallback_down(service_t *fb) {
    if (!fb->standby_up) return;
    fb->standby_up = 0;
    fb->flags |= SVC_NO_RESTART;
    if (fb->child_pid > 0) {
        fb->ctl_killed = 1;
        stop_main(fb);
    } else {
        fb->inst.state = STATE_EXCISED;
    }
    service_log(fb, "fallback-down");
}

static void park(service_t *svc) {
    struct timespec now;
    time_t delay;
    svc->dormant_count++;
    delay = 300L << (svc->dormant_count - 1);
    if (delay > 3600) delay = 3600;
    clock_gettime(CLOCK_MONOTONIC, &now);
    svc->dormant_until.tv_sec  = now.tv_sec + delay;
    svc->dormant_until.tv_nsec = now.tv_nsec;
    svc->inst.state = STATE_DORMANT;
    service_log(svc, "dormant");
}

const char *excise_reason_name(uint8_t r) {
    static const char *const n[] = {
        [EXC_NONE] = "none", [EXC_BREAKER] = "breaker", [EXC_START_TIMEOUT] = "start-timeout",
        [EXC_MAIN_PID_TIMEOUT] = "main-pid-timeout", [EXC_EXIT_CODE] = "exit-code",
        [EXC_SIGNAL] = "signal", [EXC_CORE_DUMP] = "core-dump",
        [EXC_READINESS_LOST] = "readiness-lost", [EXC_NO_CGROUP] = "no-cgroup",
        [EXC_LISTEN_FAIL] = "listen-fail", [EXC_FUSE] = "fuse", [EXC_PROBE] = "probe-failed",
        [EXC_BIND] = "bind", [EXC_TRIGGER_LIMIT] = "trigger-limit-hit",
    };
    return r < sizeof n / sizeof n[0] ? n[r] : "unknown";
}

/* A death nobody asked for, from the last wait status; a clean exit 0 is none. */
uint8_t excise_reason_from_exit(const service_t *svc) {
    if (svc->ctl_killed) return EXC_NONE;
    if (svc->term_signal) return svc->core_dumped ? EXC_CORE_DUMP : EXC_SIGNAL;
    return svc->has_exited && svc->exit_status != 0 ? EXC_EXIT_CODE : EXC_NONE;
}

/* FRICTION gave up on the service: park it with backoff, or after the fifth
 * time trip the crash-loop breaker. Only this path sets circuit_broken; a
 * start-timeout or fallback-down excise is not a crash loop. */
void engine_friction_verdict(service_t *svc) {
    if (!(svc->flags & SVC_CRITICAL) && !svc->no_excise && svc->dormant_count >= 4) {
        svc->dormant_count++;
        svc->circuit_broken = 1;
        svc->excise_reason = EXC_BREAKER;
        service_log(svc, "breaker-tripped");   /* not also 76-excised: one loss, one record */
    } else {
        park(svc);
    }
}

static int held_now(const service_t *svc) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return svc_retry_held(svc, &now);
}

/* start/restart/reset lift a stop's hold, not the .svc's own no_restart=1:
 * clearing the shared flag made a no_restart service auto-restart from then on */
static void restart_policy_restore(service_t *svc) {
    svc->path_stop = 0;
    if (svc->no_restart_cfg) svc->flags |= SVC_NO_RESTART;
    else svc->flags &= ~SVC_NO_RESTART;
}

static void ctl_rearm(service_t *svc, int start) {
    svc->card_next = 0;
    breaker_clear(svc);
    svc->excise_reason = EXC_NONE;
    svc->bind_held = 0;
    svc->restart_at.tv_sec = svc->restart_at.tv_nsec = 0;
    svc->trigger_count = 0;
    svc->trigger_window = (struct timespec){0, 0};
    if (start) svc->standby_up |= service_is_standby(svc, services, svc_count);
    if (svc->fallback_live) {
        service_t *fb = service_fallback(svc, services, svc_count);
        if (fb) fallback_down(fb);
    }
}

static void play_card(service_t *svc) {
    char ev[32];
    if (svc->card_pid > 0 || svc->card_next >= svc->card_count) return;
    snprintf(ev, sizeof ev, "card-%d/%d", svc->card_next + 1, svc->card_count);
    if (strncmp(svc->card[svc->card_next], "fallback:", 9) == 0) {
        service_t *fb = service_fallback(svc, services, svc_count);
        svc->card_next++;
        service_log(svc, ev);
        if (!fb) {
            service_log(svc, "card-no-fallback");
            return;
        }
        svc->fallback_live = 1;
        fallback_up(fb);
        park(svc);
        return;
    }
    pid_t pid = service_card_spawn(svc, svc->card_next);
    svc->card_next++;
    svc->card_killed = 0;
    if (pid < 0) {
        service_log(svc, "card-fork-fail");
        return;
    }
    svc->card_pid = pid;
    clock_gettime(CLOCK_MONOTONIC, &svc->card_start);
    service_log(svc, ev);
}

static void card_done(service_t *svc, int status) {
    int killed = svc->card_killed;
    svc->card_pid = 0;
    svc->card_killed = 0;
    if (killed) return;                 /* card-timeout was logged at the kill */
    service_log(svc, WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "card-done" : "card-failed");
}

static void kill_group(pid_t p) {
    kill(-p, SIGKILL);
    kill(p, SIGKILL);
}

static uint64_t stop_grace_ms(const service_t *svc) {
    return svc->stop_timeout_sec ? (uint64_t)svc->stop_timeout_sec * 1000 : SHUT_GRACE_MS;
}

/* exec_stop / exec_stop_post: one runner child, reaped by reap() and bounded
 * by stop_timeout_sec in monitor_failsafes(); PID 1 never waits on it.
 * stop_phase: 1 exec_stop, 2 exec_stop_post, | STOP_TIMED_OUT once killed. */
#define STOP_TIMED_OUT 4
static int stop_cmds_begin(service_t *svc, int phase) {
    int n = (phase & 1 ? svc->exec_stop_count : 0) + (phase & 2 ? svc->exec_stop_post_count : 0);
    if (!n || svc->stop_pid > 0) return 0;
    pid_t pid = service_stop_spawn(svc, phase);
    if (pid < 0) {
        service_log(svc, "stop-cmd-fork-fail");
        return 0;
    }
    svc->stop_pid = pid;
    svc->stop_phase = phase;
    clock_gettime(CLOCK_MONOTONIC, &svc->stop_start);
    service_log(svc, phase == 2 ? "stop-post" : "stop-cmd");
    return 1;
}

/* ── bind= (docs/superpowers/specs/2026-11-03-bind-key-design.md) ──────
 * The binder follows its bound: stopped when the bound goes down (held),
 * started again when it comes back, unless it was stopped on purpose or
 * failed on its own. bind=X is also a dep=X, so dep_idx resolves it. */

static int svc_is_up(const service_t *s) {
    if (s->inst.state == STATE_FUNDAMENTAL || s->inst.state == STATE_SETTLED ||
        s->inst.state == STATE_PERFECT) return 1;
    /* a socket service PID 1 listens for stays up across its idle exit and
     * the on-demand start after it: its binders mustn't churn per connection */
    return s->listen_open && (s->inst.state == STATE_NEW_PROCESS || s->inst.state == STATE_FULL_TRUST);
}

/* The bound's index; -1 when it isn't in the config, or is a timer/oneshot
 * (their PERFECT means "ran", not "up") */
static int bind_bound(const service_t *s) {
    if (!s->bind_name[0]) return -1;
    for (int k = 0; k < MAX_DEPS && s->dep_name[k][0]; k++) {
        if (strcmp(s->dep_name[k], s->bind_name) != 0) continue;
        int b = s->dep_idx[k];
        if (b < 0 || b >= svc_count || (services[b].flags & (SVC_TIMER | SVC_ONESHOT))) return -1;
        return b;
    }
    return -1;
}

static void stop_main(service_t *svc);

static int ts_after(struct timespec a, struct timespec b) {
    return a.tv_sec > b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec > b.tv_nsec);
}

/* The hold remembers the bound's run that was last seen up: "back" is a newer
 * run up. Not the time of the hold, which can come after the bound already
 * respawned (a down edge noticed late, a binder reaped after) */
static void bind_hold(service_t *b, const char *ev) {
    if (!b->bind_held) {
        int t = bind_bound(b);
        b->bind_held_since = t >= 0 ? services[t].bind_seen_spawn : (struct timespec){0, 0};
    }
    b->bind_held = 1;
    b->bind_died_at.tv_sec = b->bind_died_at.tv_nsec = 0;
    service_log(b, ev);
}

/* Stop a running binder the way schema-ctl stop would, but held: its exit is
 * the bound's doing (reap() takes it to EXCISED/EXC_BIND, no recovery) */
static void bind_stop(service_t *b, const char *ev) {
    if (b->bind_held || b->child_pid <= 0) return;
    /* being stopped on purpose already: its bound's return must not start it */
    if (b->ctl_killed && (b->flags & SVC_NO_RESTART)) return;
    bind_hold(b, ev);
    b->restart_at.tv_sec = b->restart_at.tv_nsec = 0;
    stop_main(b);
}

/* A stop was asked of SVC: its running binders go first, held, so their exit
 * during its exec_stop (podman pod stop ends the containers) isn't theirs */
static void bind_stop_binders(const service_t *svc) {
    int idx = (int)(svc - services);
    if (idx < 0 || idx >= svc_count) return;
    for (int j = 0; j < svc_count; j++)
        if (j != idx && bind_bound(&services[j]) == idx)
            bind_stop(&services[j], "bind-stop");
}

static void bind_start(service_t *b) {
    b->bind_held = 0;
    b->excise_reason = EXC_NONE;
    b->inst.state = STATE_NEW_PROCESS;
    service_log(b, "bind-start");
}

/* Once per tick pass, before the services tick: act on each bound's edge
 * since the last pass's tail (inst.prev_state isn't maintained by the
 * engine, so the edge is our own bind_was_up snapshot) */
static void bind_tick(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int j = 0; j < svc_count; j++) {
        service_t *b = &services[j];
        if (!b->bind_name[0]) {
            /* bind= removed on a reload while held: nothing to wait for now */
            if (b->bind_held) {
                b->bind_held = 0;
                if (b->inst.state == STATE_EXCISED && b->excise_reason == EXC_BIND && b->child_pid <= 0)
                    bind_start(b);
            }
            continue;
        }
        int t = bind_bound(b);
        if (t < 0) {
            /* no bound to follow: never runs without it */
            if (!b->bind_held) {
                if (b->child_pid > 0) bind_stop(b, "bind-target-missing");
                else {
                    bind_hold(b, "bind-target-missing");
                    if (b->inst.state != STATE_EXCISED) {
                        b->inst.state = STATE_EXCISED;
                        b->excise_reason = EXC_BIND;
                    }
                }
            }
            continue;
        }
        service_t *bd = &services[t];
        int up = svc_is_up(bd);
        /* gone since the last pass, or back already with a new process */
        int went_down = bd->bind_was_up &&
            (!up || (!bd->listen_open && (bd->spawn_time_mono.tv_sec != bd->bind_seen_spawn.tv_sec ||
                                          bd->spawn_time_mono.tv_nsec != bd->bind_seen_spawn.tv_nsec)));
        if (went_down) {
            if (b->child_pid > 0) {
                bind_stop(b, "bind-stop");
            } else if (!b->bind_held && !b->circuit_broken &&
                       (b->bind_died_at.tv_sec || b->bind_died_at.tv_nsec)) {
                int64_t ago = (int64_t)(now.tv_sec - b->bind_died_at.tv_sec) * 1000 +
                              (now.tv_nsec - b->bind_died_at.tv_nsec) / 1000000;
                if (ago >= 0 && ago < BIND_GRACE_MS) {
                    /* it went first in the bound's own going: not its failure */
                    b->restart_count = b->bind_saved_restarts;
                    b->dormant_count = b->bind_saved_dormant;
                    b->restart_at.tv_sec = b->restart_at.tv_nsec = 0;
                    b->inst.state = STATE_EXCISED;
                    b->excise_reason = EXC_BIND;
                    bind_hold(b, "bind-reclassified");
                }
            }
        }
        /* back: up, not stopping, and a run of it newer than our hold. Up
         * alone isn't enough -- a pod asked to stop is still FUNDAMENTAL
         * while its exec_stop ends our process */
        int back = up && bd->stop_pid <= 0 && !bd->ctl_killed &&
                   ts_after(bd->spawn_time_mono, b->bind_held_since);
        /* and not before its own old process and stop runner are gone */
        if (b->bind_held && back && b->child_pid <= 0 && b->stop_pid <= 0)
            bind_start(b);
        /* strict start: dep= lets a non-critical excised dep through; bind= never */
        if (!b->bind_held && b->child_pid <= 0 && b->inst.state == STATE_NEW_PROCESS &&
            (bd->inst.state == STATE_EXCISED || bd->inst.state == STATE_DORMANT)) {
            b->inst.state = STATE_EXCISED;
            b->excise_reason = EXC_BIND;
            bind_hold(b, "bind-wait");
        }
    }
}

static void bind_snapshot_up(void) {
    for (int j = 0; j < svc_count; j++) {
        services[j].bind_was_up = (uint8_t)svc_is_up(&services[j]);
        if (services[j].bind_was_up) services[j].bind_seen_spawn = services[j].spawn_time_mono;
    }
}

/* SIGTERM, or "signal 37" for one with no short name */
static void sig_text(int sig, char *buf, size_t n) {
    const char *ab = sig_abbrev(sig);
    if (ab) snprintf(buf, n, "SIG%s", ab);
    else snprintf(buf, n, "signal %d", sig);
}

static int kill_sig(const service_t *s) { return s->kill_signal ? s->kill_signal : SIGTERM; }
static int final_sig(const service_t *s) { return s->final_kill_signal ? s->final_kill_signal : SIGKILL; }

/* a runtime stop that never ends: final_kill_signal after stop_timeout_sec,
 * else systemd's TimeoutStopSec default (shutdown keeps its own 3 s grace) */
#define RUNTIME_STOP_GRACE_MS 90000

static uint64_t runtime_stop_grace_ms(const service_t *svc) {
    return svc->stop_timeout_sec ? (uint64_t)svc->stop_timeout_sec * 1000 : RUNTIME_STOP_GRACE_MS;
}

static int kill_sent_set(const service_t *svc) {
    return svc->kill_sent.tv_sec || svc->kill_sent.tv_nsec;
}

/* kill_signal= to the whole cgroup under control-group, else the main
 * process. The escalation deadline runs from the first signal: a repeated
 * stop doesn't push it back. */
static void stop_signal(service_t *svc) {
    if (svc->child_pid <= 0) return;
    if (svc->kill_mode == KILL_MODE_CGROUP) svc_signal_all(svc, kill_sig(svc));
    else kill(svc->child_pid, kill_sig(svc));
    if (!kill_sent_set(svc)) clock_gettime(CLOCK_MONOTONIC, &svc->kill_sent);
}

/* final_kill_signal=: the main process under process, else everything */
static void final_signal(service_t *svc) {
    int sig = final_sig(svc);
    if (svc->kill_mode == KILL_MODE_PROCESS) {
        if (svc->child_pid > 0) kill(svc->child_pid, sig);
    } else if (sig == SIGKILL) {
        svc_cgroup_kill_tree(svc);
        if (svc->child_pid > 0) kill(svc->child_pid, SIGKILL);
    } else {
        svc_signal_all(svc, sig);
    }
}

/* What the main process left when it went, by kill_mode: unset sweeps it
 * (SIGKILL), mixed sends final_kill_signal, process leaves it, and
 * control-group leaves a stop's TERMed rest its grace (kill_sent stays set:
 * the escalation check sends final_kill_signal at the deadline, and a
 * respawn waits for the cgroup to drain). 1 = still draining. */
static int sweep_rest(service_t *svc) {
    switch (svc->kill_mode) {
    case KILL_MODE_PROCESS:
        return 0;
    case KILL_MODE_MIXED:
        final_signal(svc);
        return 0;
    case KILL_MODE_CGROUP:
        if (kill_sent_set(svc) && svc_signal_all(svc, 0) > 0) return 1;
        svc_cgroup_kill_tree(svc);
        return 0;
    default:
        svc_cgroup_kill_tree(svc);
        return 0;
    }
}

/* Stop signal to the main process, after its exec_stop commands when it has any */
static void stop_main(service_t *svc) {
    if (svc->child_pid <= 0) return;
    bind_stop_binders(svc);
    if (svc->stop_pid > 0 && svc->stop_phase == 1) return;
    if (!stop_cmds_begin(svc, 1)) stop_signal(svc);
}

static void stop_cmds_done(service_t *svc, int status) {
    int phase = svc->stop_phase;
    svc->stop_pid = 0;
    svc->stop_phase = 0;
    service_log(svc, WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "stop-cmd-done" : "stop-cmd-failed");
    if ((phase & 3) != 1) return;
    if (svc->child_pid > 0) {
        if (!(phase & STOP_TIMED_OUT)) stop_signal(svc);
    } else {
        /* main went first (exec_stop killed it): its post was held for us */
        stop_cmds_begin(svc, 2);
    }
}

/* exec_reload runner gone: the service itself is untouched either way */
static void reload_cmds_done(service_t *svc, int status) {
    svc->reload_pid = 0;
    service_log(svc, WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "reload-done" : "reload-fail");
}

static void mark_boot_ready(service_t *svc, int how) {
    if (svc->boot_ready.tv_sec || svc->boot_ready.tv_nsec) return;
    svc->boot_ready = svc->stable_time;
    svc->boot_how = how;
}

/* Record why a run ended (wait status) and log a signal death to the rail. */
static void note_exit(service_t *svc, int status) {
    svc->exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    svc->term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    svc->core_dumped = WIFSIGNALED(status) && WCOREDUMP(status);
    svc->has_exited  = 1;
    if (svc->term_signal) {
        char ev[48];
        const char *ab = sig_abbrev(svc->term_signal);
        snprintf(ev, sizeof ev, "killed-SIG%s%s", ab ? ab : "?", svc->core_dumped ? "-core" : "");
        service_log(svc, ev);
    }
}

/* A forking daemon's main PID, from its pid_file: alive and inside the
 * service's cgroup, so a stale or planted file cannot hand PID 1 some other
 * process to supervise and kill. 0 if not (yet) valid. */
static pid_t pid_file_read(const service_t *svc) {
    char buf[32];
    struct stat st;
    long p = 0;
    int fd, found = 0;
    ssize_t n;

    fd = open(svc->pid_file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return 0; }
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    if (sscanf(buf, "%ld", &p) != 1) return 0;
    if (p <= 1 || p > INT_MAX || kill((pid_t)p, 0) != 0) return 0;
    if (!svc->cgroup_path[0]) return 0;
    /* in the cgroup or below it: a daemon may move itself into a child */
    char text[NOTIFY_CGROUP_TEXT_MAX];
    found = notify_read_cgroup((pid_t)p, text, sizeof text) &&
            notify_cgroup_text_matches(text, svc->cgroup_path);
    return found ? (pid_t)p : 0;
}

/* The launcher of a pid_file service exited 0: adopt the daemon it left, or
 * wait up to PID_FILE_WAIT_SECS for a pid_file written after the exit. */
static void fork_adopt(service_t *svc) {
    pid_t p = pid_file_read(svc);
    if (p) {
        char ev[48];
        svc->child_pid  = p;
        svc->fork_state = 1;
        snprintf(ev, sizeof ev, "forked pid=%d", (int)p);
        service_log(svc, ev);
        return;
    }
    if (svc->fork_state == 0) {
        svc->fork_state = -1;
        clock_gettime(CLOCK_MONOTONIC, &svc->fork_wait);
        svc->fork_wait.tv_sec += PID_FILE_WAIT_SECS;
    }
}

#define LISTEN_MIN_RUN_SECS 2   /* a quicker exit served nothing: recovery arc, not a wake loop */

static void path_after_run(service_t *s);
static int dynuser_assign(service_t *s);
static void dynuser_apply_all(void);
static void path_gc(void);

/* timer fire complete: re-arm regardless of exit code (cron semantics — a
 * failed or condition-skipped run isn't retried, it runs next window).
 * interval 0 = run-once (only on_boot_sec): drop the flag so PERFECT becomes
 * terminal instead of re-firing every tick. */
static void timer_rearm(service_t *svc) {
    svc->inst.state = STATE_PERFECT;
    svc->timer_catchup_wait = 0;
    if (svc->flags & SVC_TIMER_CALENDAR) {
        if (svc->flags & SVC_TIMER_PERSIST)
            timer_stamp_write(svc, time(NULL));
        timer_arm_calendar(svc);
    } else if (svc->timer_interval_sec > 0) {
        struct timespec tn;
        clock_gettime(CLOCK_MONOTONIC, &tn);
        svc->timer_next = tn;
        svc->timer_next.tv_sec += svc->timer_interval_sec;
        timer_jitter(svc);
    } else {
        svc->flags &= ~SVC_TIMER;
    }
}

/* MAINPID=: alive and inside the service's cgroup or below it. Not
 * pid_file_read()'s scan of the top-level cgroup.procs: a delegated service
 * keeps that empty and its processes in children. */
static const char *mainpid_why(const service_t *svc, pid_t p) {
    char text[NOTIFY_CGROUP_TEXT_MAX];
    if (p <= 1) return "invalid";
    if (kill(p, 0) != 0) return "gone";
    if (!svc->cgroup_path[0]) return "no-cgroup";
    if (!notify_read_cgroup(p, text, sizeof text) ||
        !notify_cgroup_text_matches(text, svc->cgroup_path)) return "outside-cgroup";
    return NULL;
}

static void mainpid_rejected(service_t *svc, pid_t p, const char *why) {
    char ev[64];
    if (p == svc->main_rejected) return;
    svc->main_rejected = p;
    snprintf(ev, sizeof ev, "main-pid-rejected pid=%d %s", (int)p, why);
    service_log(svc, ev);
}

/* the pid's parent: only our own children (orphans reparented to PID 1 or
 * to schema-user, a subreaper) are ever reaped by us */
static pid_t pid_parent(pid_t p) {
    char path[64], buf[512], *rp;
    int fd, ppid = 0;
    ssize_t n;
    snprintf(path, sizeof path, "/proc/%d/stat", (int)p);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) return 0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    if (!(rp = strrchr(buf, ')')) || sscanf(rp + 1, " %*c %d", &ppid) != 1) return 0;
    return (pid_t)ppid;
}

/* ctl stop/restart of a service with no child yet: drop a forking or
 * MAINPID= wait. Only the MAINPID= wait gets post, as its timeout does;
 * the pid_file timeout runs none. */
static void ctl_wait_abort(service_t *svc) {
    if (svc->fork_state >= 0 && !svc->main_wait) return;
    int post = svc->main_wait;
    svc_cgroup_kill_tree(svc);
    svc->fork_state = 0;
    svc->main_wait = 0;
    if (post) stop_cmds_begin(svc, 2);
}

static void main_adopt(service_t *svc, pid_t p) {
    char ev[48];
    svc->child_pid = svc->main_pid = p;
    svc->main_wait = 0;
    snprintf(ev, sizeof ev, "main-pid pid=%d", (int)p);
    service_log(svc, ev);
}

static void notify_poll(int max_batch);

static void reap(void) {
    int status;
    pid_t pid;
    int i;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* read what the dead sent before handling its death, so an FDSTORE
         * sent just before exit lands before the exit drops the store */
        if (notify_fd >= 0) notify_poll(NOTIFY_DRAIN_MAX);
        if (engine_reexec_reaped(pid, status) || engine_hooks.on_reaped(pid, status))
            continue;
        if (fdclose_reaped(pid)) continue;
        for (i = 0; i < svc_count; i++) {
            if (services[i].card_pid == pid) {
                card_done(&services[i], status);
                break;
            }
            if (services[i].stop_pid == pid) {
                stop_cmds_done(&services[i], status);
                break;
            }
            if (services[i].reload_pid == pid) {
                reload_cmds_done(&services[i], status);
                break;
            }
            if (services[i].child_pid != pid) continue;

            services[i].last_pid   = pid;
            services[i].child_pid  = 0;
            note_exit(&services[i], status);

            /* bind=: an exit while its bound is down or stopping is the bound's
             * doing; one while the bound is up is its own, remembered in case
             * the bound follows within BIND_GRACE_MS (crash order varies) */
            if (services[i].bind_name[0] && !services[i].bind_held && !services[i].ctl_killed) {
                int t = bind_bound(&services[i]);
                if (t < 0 || !svc_is_up(&services[t]) || services[t].ctl_killed ||
                    (services[t].stop_pid > 0 && (services[t].stop_phase & 3) == 1)) {
                    bind_hold(&services[i], "bind-stop");
                } else if (services[i].term_signal ||
                           (services[i].has_exited && service_death_class(&services[i]) != DEATH_CLEAN)) {
                    /* a success_exit_status= code is as clean as exit 0 here */
                    /* a clean exit was its own to make; only a failure can be the bound's */
                    clock_gettime(CLOCK_MONOTONIC, &services[i].bind_died_at);
                    services[i].bind_saved_restarts = services[i].restart_count;
                    services[i].bind_saved_dormant = services[i].dormant_count;
                }
            }
            if (services[i].bind_held) {
                /* not a death: no restart budget, failsafe, recovery or breaker */
                services[i].inst.state = STATE_EXCISED;
                services[i].excise_reason = EXC_BIND;
                services[i].main_wait = 0;
                if (!sweep_rest(&services[i])) services[i].kill_sent = (struct timespec){0, 0};
                service_log(&services[i], "bind-stopped");
                stop_cmds_begin(&services[i], 2);
                break;
            }

            /* main_pid_notify: the launcher exiting is a handover, not a death */
            /* only a launcher: a daemon that sent READY=1 itself and then exits
             * 0 has simply ended, and timers, oneshots and socket services
             * keep their own exit handling */
            if ((services[i].flags & SVC_MAIN_PID_NOTIFY) && pid != services[i].main_pid &&
                !services[i].ctl_killed &&
                !(services[i].flags & (SVC_TIMER | SVC_ONESHOT)) && !services[i].listen_open &&
                (services[i].main_pid || !services[i].notify_ready)) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && !services[i].cgroup_path[0]) {
                    /* nothing to check a MAINPID= against: refuse, as pid_file does */
                    services[i].inst.state = STATE_EXCISED;
                    services[i].excise_reason = EXC_NO_CGROUP;
                    service_log(&services[i], "main-pid-no-cgroup");
                    stop_cmds_begin(&services[i], 2);
                    break;
                }
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    pid_t mp = services[i].main_pid;
                    const char *why = mp ? mainpid_why(&services[i], mp) : NULL;
                    if (mp && !why && pid_parent(mp) != getpid()) why = "not-ours";
                    if (mp && !why) {
                        main_adopt(&services[i], mp);
                    } else {
                        if (why) mainpid_rejected(&services[i], mp, why);
                        struct timespec t;
                        clock_gettime(CLOCK_MONOTONIC, &t);
                        t.tv_sec += MAINPID_WAIT_SECS;
                        services[i].main_pid = 0;
                        services[i].main_wait = 1;
                        services[i].main_wait_due = t;
                    }
                    break;
                }
                /* a failed launcher takes what it started with it */
                svc_cgroup_kill_tree(&services[i]);
            }
            services[i].main_pid = 0;
            /* a service that made cgroups of its own (podman's runtime/ and
             * payload) leaves <svc> a non-leaf: the next run could not join
             * it, so any exit takes the whole tree (systemd's KillMode does
             * too) — except a forking launcher's exit 0, which hands over */
            int handover = services[i].pid_file[0] && services[i].fork_state == 0 &&
                           services[i].inst.state == STATE_FULL_TRUST && !services[i].ctl_killed &&
                           WIFEXITED(status) && WEXITSTATUS(status) == 0;
            if (!handover && services[i].cgroup_path[0] && cgroup_has_children(services[i].cgroup_path))
                svc_cgroup_kill_tree(&services[i]);

            int death = service_death_class(&services[i]);
            if (services[i].flags & SVC_PATH) {
                path_after_run(&services[i]);
                service_log(&services[i], death == DEATH_CLEAN ? "path-done" : "path-failed");
                stop_cmds_begin(&services[i], 2);
                break;
            }
            if (services[i].flags & SVC_TIMER) {
                timer_rearm(&services[i]);
                service_log(&services[i], death == DEATH_CLEAN ? "timer-done" : "timer-failed");
                stop_cmds_begin(&services[i], 2);
                break;
            }

            if (services[i].pid_file[0] && services[i].fork_state == 0 &&
                services[i].inst.state == STATE_FULL_TRUST &&
                !services[i].ctl_killed &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                if (!services[i].cgroup_path[0]) {
                    /* nothing to prove the PID is ours or to find a daemon
                     * left behind by: refuse rather than retry into duplicates */
                    services[i].inst.state = STATE_EXCISED;
                    services[i].excise_reason = EXC_NO_CGROUP;
                    service_log(&services[i], "pid_file-no-cgroup");
                    break;
                }
                fork_adopt(&services[i]);
                break;
            }

            if (services[i].listen_open && !services[i].ctl_killed &&
                !(services[i].flags & SVC_ONESHOT) &&
                (death == DEATH_CLEAN || (services[i].flags & SVC_NO_RESTART)) &&
                mono_sec() - services[i].spawn_time_mono.tv_sec >= LISTEN_MIN_RUN_SECS) {
                /* idle exit of a socket service: PID 1 still holds its
                 * sockets, the next connection starts it again */
                services[i].inst.state = STATE_PERFECT;
                clock_gettime(CLOCK_MONOTONIC, &services[i].stable_time);
                service_log(&services[i], "socket-idle");
            } else if (death == DEATH_CLEAN && (services[i].flags & SVC_ONESHOT)) {
                /* exit 0, a clean signal, or what success_exit_status= lists */
                /* clean one-shot exit → PERFECT */
                services[i].inst.state = STATE_PERFECT;
                clock_gettime(CLOCK_MONOTONIC, &services[i].stable_time);
                mark_boot_ready(&services[i], READY_EXIT);
                service_log(&services[i], "oneshot-done");
                if (strcmp(services[i].name, "slot-detect") == 0) {
                    load_env_file();
                }
            } else if ((services[i].flags & SVC_NO_RESTART) ||
                       (!services[i].ctl_killed && !service_restart_wanted(&services[i], death))) {
                /* held down, or restart= / restart_prevent_exit_status= say
                 * this exit stays down: EXCISED, as no_restart=1 always was */
                services[i].inst.state = STATE_EXCISED;
                /* a success_exit_status= code reads as stopped, not exit-code;
                 * signals keep their reason, as always */
                services[i].excise_reason = !services[i].term_signal && death == DEATH_CLEAN
                                            ? EXC_NONE : excise_reason_from_exit(&services[i]);
                service_log(&services[i], (services[i].flags & SVC_NO_RESTART) ? "76-no-restart" : "76-restart-policy");
                /* Explicit stop/down lands here straight from reap() — it
                 * never crosses the FRICTION→EXCISED transition that the tick
                 * loop uses to tear cgroups down, and case STATE_EXCISED is a
                 * no-op. Without this an isolated cpuset partition keeps its
                 * core carved out of general scheduling (and leaks the parent
                 * cpuset.cpus.exclusive union) until reboot. Release here so
                 * stop frees the reservation; cgroup_assign() rebuilds the
                 * cgroup on any later start. kill_mode decides about the
                 * rest (process: sshd's stop keeps the sessions). */
                if (!sweep_rest(&services[i])) services[i].kill_sent = (struct timespec){0, 0};
            } else if (services[i].ctl_killed) {
                /* unset: only a forking / MAINPID= service's leftovers are swept
                 * (as before); an explicit kill_mode decides for itself */
                if (services[i].kill_mode != KILL_MODE_DEFAULT ||
                    services[i].pid_file[0] || (services[i].flags & SVC_MAIN_PID_NOTIFY)) {
                    if (!sweep_rest(&services[i])) services[i].kill_sent = (struct timespec){0, 0};
                } else {
                    services[i].kill_sent = (struct timespec){0, 0};
                }
                services[i].inst.state = STATE_NEW_PROCESS;
                service_log(&services[i], "ctl-restart");
            } else {
                /* unexpected death → enter recovery arc; an explicit
                 * control-group/mixed takes what the main process left */
                if (services[i].kill_mode == KILL_MODE_CGROUP || services[i].kill_mode == KILL_MODE_MIXED)
                    sweep_rest(&services[i]);
                services[i].kill_sent = (struct timespec){0, 0};
                restart_budget_refresh(&services[i]);
                if (services[i].failsafe_cmd[0]) {
                    start_failsafe(&services[i]);
                }
                services[i].inst.state = STATE_RECOVERY;
                service_log(&services[i], "died");
            }
            stop_cmds_begin(&services[i], 2);
            break;
        }
        /* only a pid no live service claims: an entry adopted from an older
         * image may name a pid it already reaped, now reused by a service */
        if (i == svc_count) eviction_reaped(pid);
    }
}

uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void execute_fuse_cmd(service_t *svc) {
    pid_t pid;
    if (!svc->fuse_cmd[0]) return;
    service_log(svc, "fuse-cmd-exec");
    pid = fork();
    if (pid == 0) {
        service_reset_child_sigmask();
        setsid();
        execl("/bin/sh", "sh", "-c", svc->fuse_cmd, NULL);
        _exit(127);
    }
}

static void start_failsafe(service_t *svc) {
    pid_t pid;
    if (!svc->failsafe_cmd[0]) return;
    if (svc->failsafe_pid > 0) return; /* already running */

    service_log(svc, "failsafe-exec");
    pid = fork();
    if (pid < 0) {
        service_log(svc, "failsafe-fork-fail");
        return;
    }
    if (pid == 0) {
        service_reset_child_sigmask();
        setsid();
        char *at = strchr(svc->name, '@');
        if (at) {
            if (*(at + 1)) {
                setenv("INSTANCE", at + 1, 1);
            } else {
                char *slot = getenv("SLOT_ID");
                if (slot) {
                    setenv("INSTANCE", slot, 1);
                }
            }
        }
        execl("/bin/sh", "sh", "-c", svc->failsafe_cmd, NULL);
        _exit(127);
    }
    
    svc->failsafe_pid = pid;
    clock_gettime(CLOCK_MONOTONIC, &svc->failsafe_start);
}

static void monitor_failsafes(void) {
    int i;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    for (i = 0; i < svc_count; i++) {
        service_t *svc = &services[i];
        if (svc->card_pid > 0 && !svc->card_killed && now.tv_sec - svc->card_start.tv_sec >= CARD_TIMEOUT_SECS) {
            /* never wait for it here: a card stuck in D state outlives SIGKILL,
             * and a blocking waitpid would stop the loop that feeds the watchdog */
            kill(-svc->card_pid, SIGKILL);
            kill(svc->card_pid, SIGKILL);
            svc->card_killed = 1;
            service_log(svc, "card-timeout");
        }
        if (svc->main_wait && now.tv_sec >= svc->main_wait_due.tv_sec) {
            svc->main_wait = 0;
            svc_cgroup_kill_tree(svc);
            service_log(svc, "main-pid-timeout");
            restart_budget_refresh(svc);
            if (svc->failsafe_cmd[0]) start_failsafe(svc);
            svc->inst.state = (svc->flags & SVC_NO_RESTART) || !service_restart_wanted(svc, DEATH_TIMEOUT)
                              ? STATE_EXCISED : STATE_RECOVERY;
            if (svc->inst.state == STATE_EXCISED) svc->excise_reason = EXC_MAIN_PID_TIMEOUT;
            stop_cmds_begin(svc, 2);
        }
        if (svc->stop_pid > 0 && !(svc->stop_phase & STOP_TIMED_OUT)) {
            uint64_t ran = (uint64_t)(now.tv_sec - svc->stop_start.tv_sec) * 1000 +
                           (now.tv_nsec - svc->stop_start.tv_nsec) / 1000000;
            if (ran >= stop_grace_ms(svc)) {
                kill_group(svc->stop_pid);
                service_log(svc, "stop-cmd-timeout");
                /* signal the main process now; reap() still collects the runner */
                if (svc->stop_phase == 1 && svc->child_pid > 0) stop_signal(svc);
                svc->stop_phase |= STOP_TIMED_OUT;
            }
        }
        if (kill_sent_set(svc)) {
            uint64_t ran = (uint64_t)(now.tv_sec - svc->kill_sent.tv_sec) * 1000 +
                           (now.tv_nsec - svc->kill_sent.tv_nsec) / 1000000;
            int draining = svc->child_pid <= 0;
            if (draining && svc_signal_all(svc, 0) == 0) {
                /* control-group's rest went within its grace */
                svc_cgroup_kill_tree(svc);
                svc->kill_sent = (struct timespec){0, 0};
            } else if (ran >= runtime_stop_grace_ms(svc)) {
                service_log(svc, "stop-escalate");
                final_signal(svc);
                if (draining) svc_cgroup_kill_tree(svc);
                svc->kill_sent = (struct timespec){0, 0};
            }
        }
        if (svc->reload_pid > 0) {
            uint64_t ran = (uint64_t)(now.tv_sec - svc->reload_start.tv_sec) * 1000 +
                           (now.tv_nsec - svc->reload_start.tv_nsec) / 1000000;
            /* killed once: reap() clears reload_pid when it goes */
            if (ran >= stop_grace_ms(svc) && svc->reload_start.tv_sec >= 0) {
                kill_group(svc->reload_pid);
                service_log(svc, "reload-timeout");
                svc->reload_start.tv_sec = -1;
            }
        }
        if (svc->failsafe_pid <= 0) continue;
        
        int status;
        pid_t res = waitpid(svc->failsafe_pid, &status, WNOHANG);
        if (res == svc->failsafe_pid || (res < 0 && errno == ECHILD)) {
            service_log(svc, "failsafe-done");
            svc->failsafe_pid = 0;
        } else {
            long elapsed_ms = (now.tv_sec - svc->failsafe_start.tv_sec) * 1000 +
                              (now.tv_nsec - svc->failsafe_start.tv_nsec) / 1000000;
            int timeout_ms = svc->failsafe_timeout_ms > 0 ? svc->failsafe_timeout_ms : 500;
            if (elapsed_ms >= timeout_ms) {
                service_log(svc, "failsafe-timeout");
                kill(svc->failsafe_pid, SIGKILL);
                /* don't block PID 1 waiting on it — the WNOHANG poll at the top
                 * of this loop reaps it on the next tick. */
            }
        }
    }
}

static void active_kill_service(service_t *svc) {
    if (svc->child_pid <= 0) return;
    restart_budget_refresh(svc);
    
    kill(svc->child_pid, kill_sig(svc));
    
    int elapsed_ms = 0;
    int status;
    while (elapsed_ms < 50) {
        pid_t res = waitpid(svc->child_pid, &status, WNOHANG);
        if (res == svc->child_pid) {
            note_exit(svc, status);
            svc->child_pid = 0;
            if (svc->kill_mode != KILL_MODE_PROCESS) svc_cgroup_kill_tree(svc);
            stop_cmds_begin(svc, 2);
            return;
        }
        usleep(1000);
        elapsed_ms++;
    }
    
    kill(svc->child_pid, SIGKILL);
    /* bounded non-blocking reap: never block PID 1 on a process wedged in
     * uninterruptible (D) state — e.g. a deadlocked SPI/NFS driver. If it
     * doesn't die within the window, leave the zombie for the signalfd
     * reaper and proceed so the main loop can't hang. */
    elapsed_ms = 0;
    int reaped = 0;
    while (elapsed_ms < 50) {
        if (waitpid(svc->child_pid, &status, WNOHANG) == svc->child_pid) { reaped = 1; break; }
        usleep(1000);
        elapsed_ms++;
    }
    /* A zombie left for the reaper no longer matches child_pid there, so the
     * cause is recorded now: the SIGKILL we just sent. */
    note_exit(svc, reaped ? status : SIGKILL);
    svc->child_pid = 0;
    if (svc->kill_mode != KILL_MODE_PROCESS) svc_cgroup_kill_tree(svc);
    stop_cmds_begin(svc, 2);
}

/* ── schema tick for one service ────────────────────────────────────── */

/* A socket file can outlive its listener (the bus died), so only a completed
 * connect counts. A listener whose backlog is full refuses with EAGAIN and is
 * retried on the next pass. */
static int socket_accepts(const char *path) {
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), ok;
    if (fd < 0) return 0;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof a.sun_path) { close(fd); return 0; }
    memcpy(a.sun_path, path, n + 1);
    ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

/* sock_waiting is only recomputed in NEW_PROCESS; a held service that is
 * stopped or excised keeps the stale flag. */
int svc_held_on_socket(const service_t *svc) {
    return svc->sock_waiting && svc->inst.state == STATE_NEW_PROCESS;
}

/* A ready_path counts only once it is not the file that was already there
 * at spawn: a marker or socket left by a crashed run must not promote the new
 * one. ctime catches a rewrite in place and a new file on a reused inode. */
static int ready_path_fresh(const service_t *svc) {
    struct stat st;
    if (stat(svc->ready_path, &st) != 0) return 0;
    return !(svc->ready_stale_ino && st.st_ino == svc->ready_stale_ino &&
             st.st_dev == svc->ready_stale_dev &&
             st.st_ctim.tv_sec == svc->ready_stale_ctime.tv_sec &&
             st.st_ctim.tv_nsec == svc->ready_stale_ctime.tv_nsec);
}

/* ── ready_path liveness ─────────────────────────────────────────────
 * A verified ready_path is watched with inotify on its directory instead of
 * polled every tick. Any event, including IN_Q_OVERFLOW and IN_IGNORED,
 * schedules one recheck of every watched service READY_EVENT_GRACE_MS later.
 * A service that unlinks its socket on the way out is reaped by then, so a
 * crash takes the normal restart path, not readiness-lost. READY_BACKSTOP_MS
 * rechecks catch whatever inotify cannot see (bind mounts, odd filesystems). */
#define READY_EVENT_GRACE_MS  100
#define READY_BACKSTOP_MS     30000

static int ready_live(const service_t *svc) {
    return svc->inst.state == STATE_FUNDAMENTAL && svc->ready_path[0] &&
           svc->child_pid > 0 && !svc->ctl_killed && svc->ready_path_verified;
}

static void ready_lost(service_t *svc) {
    service_log(svc, "readiness-lost");
    active_kill_service(svc);
    if ((svc->flags & SVC_NO_RESTART) || !service_restart_wanted(svc, DEATH_TIMEOUT)) {
        svc->inst.state = STATE_EXCISED;
        svc->excise_reason = EXC_READINESS_LOST;
        service_log(svc, (svc->flags & SVC_NO_RESTART) ? "76-no-restart" : "76-restart-policy");
        return;
    }
    start_failsafe(svc);
    svc->inst.state = STATE_RECOVERY;
}

static void ready_watch(service_t *svc) {
    char dir[sizeof(svc->ready_path)];
    char *slash;
    snprintf(dir, sizeof(dir), "%s", svc->ready_path);
    slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = '\0';
    else if (slash) *slash = '\0';
    svc->ready_watched = (ino_fd >= 0 && slash &&
        inotify_add_watch(ino_fd, dir, IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF |
                                       IN_MOVE_SELF | IN_UNMOUNT) >= 0) ? 1 : -1;
    if (svc->ready_watched > 0 && access(svc->ready_path, F_OK) != 0)
        ready_lost(svc);
}

static void ready_recheck_all(void) {
    int i;
    for (i = 0; i < svc_count; i++) {
        service_t *svc = &services[i];
        if (!ready_live(svc) || svc->ready_watched <= 0 || svc->ready_poll_hz > 0)
            continue;
        if (access(svc->ready_path, F_OK) != 0)
            ready_lost(svc);
        else
            svc->ready_watched = 0;   /* re-arm: the event may have been IN_IGNORED/IN_MOVE_SELF */
    }
}

/* A fire while the service runs: an edge event is remembered for one more
 * run (the file changed after it was read); a level one is re-checked when
 * the run ends anyway. Outside PERFECT (stopped, parked, queued) it's moot. */
static void path_mark(service_t *s, int k) {
    if (s->child_pid > 0) {
        if (!watch_is_level(&s->watch[k])) s->path_rerun = 1;
    } else if (s->inst.state == STATE_PERFECT) {
        s->path_due = 1;
        s->path_which = (uint8_t)k;
    }
}

static void path_event(service_t *s, const struct inotify_event *ev) {
    for (int k = 0; k < s->watch_count; k++) {
        int e = watch_event(&s->watch[k], ev);
        if (e & WATCH_EV_REARM) {
            e |= watch_rearm(path_fd, &s->watch[k]);
            path_gc_due = 1;
        }
        if (e & WATCH_EV_FIRE) path_mark(s, k);
    }
}

/* make_directory, watches on the current path_fd, and a level watch that
 * already holds fires. Born armed (PERFECT) only the first time: a reload
 * or re-exec keeps a running or parked service's state. */
static void path_arm(service_t *s) {
    if (!s->path_armed) {
        /* a running service that just gained watch_path= finishes its run first */
        if (s->child_pid <= 0) s->inst.state = STATE_PERFECT;
        s->path_armed = 1;
    }
    for (int k = 0; k < s->watch_count; k++) {
        watch_t *w = &s->watch[k];
        if (s->make_directory && watch_make_dir(w, s->directory_mode) != 0)
            elog(stderr, "%s: make_directory for %s: %s\n", s->name, w->path, strerror(errno));
        if (path_fd >= 0) watch_arm(path_fd, w);
        if (watch_is_level(w) && watch_level_true(w)) path_mark(s, k);
    }
}

/* backstop: what inotify can't see (bind mounts, network filesystems) */
static void path_sweep(void) {
    for (int i = 0; i < svc_count; i++) {
        service_t *s = &services[i];
        if (!(s->flags & SVC_PATH)) continue;
        for (int k = 0; k < s->watch_count; k++)
            if (path_fd >= 0 && (watch_rearm(path_fd, &s->watch[k]) & WATCH_EV_FIRE)) path_mark(s, k);
    }
    path_gc();
}

static void path_fire(service_t *s) {
    struct timespec now;
    char ev[300];
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!s->trigger_window.tv_sec || now.tv_sec - s->trigger_window.tv_sec >= s->trigger_interval_sec) {
        s->trigger_window = now;
        s->trigger_count = 0;
    }
    if (++s->trigger_count > s->trigger_burst) {
        s->inst.state = STATE_EXCISED;
        s->excise_reason = EXC_TRIGGER_LIMIT;
        service_log(s, "path-trigger-limit");
        return;
    }
    s->inst.state = STATE_NEW_PROCESS;
    s->path_last_fire = time(NULL);
    snprintf(ev, sizeof ev, "path-fire %s:%s", watch_kind_name(s->watch[s->path_which].kind),
             s->watch[s->path_which].path);
    service_log(s, ev);
}

/* run over: armed again, or parked for good if it was stopped; a level
 * watch still holding, a queued edge event or a restart runs it again */
static void path_after_run(service_t *s) {
    int due = s->path_rerun || (s->ctl_killed && !s->path_stop);
    s->path_rerun = 0;
    if (s->path_stop) {
        s->inst.state = STATE_EXCISED;
        s->excise_reason = EXC_NONE;
        return;
    }
    s->inst.state = STATE_PERFECT;
    for (int k = 0; k < s->watch_count; k++)
        if (watch_is_level(&s->watch[k]) && watch_level_true(&s->watch[k])) {
            due = 1;
            s->path_which = (uint8_t)k;
        }
    s->path_due = (uint8_t)due;
}

/* dynamic_user=1: a uid no passwd/group entry and no other service holds;
 * kept once picked (it rides reloads and re-execs), and a static user= that
 * exists wins, as in systemd */
struct dynuser_ctx { const service_t *self; uint8_t map[DYNUSER_MAP_BYTES]; };

static int dynuser_taken(uid_t u, void *ctx) {
    struct dynuser_ctx *c = ctx;
    if (dynuser_mapped(c->map, u)) return 1;
    for (int i = 0; i < svc_count; i++)
        if (&services[i] != c->self && services[i].dyn_uid == (int)u) return 1;
    return 0;
}

/* a user= that resolves (name or numeric uid) is the static user, which wins */
static int dynuser_static(const service_t *s, uid_t *uid) {
    char *end;
    struct passwd *pw;
    if (!s->run_user[0]) return 0;
    unsigned long id = strtoul(s->run_user, &end, 10);
    pw = *end ? getpwnam(s->run_user) : getpwuid((uid_t)id);
    if (!pw) return 0;
    *uid = pw->pw_uid;
    return 1;
}

static int dynuser_assign(service_t *s) {
    if (!s->dynamic_user) return 0;
    if (!engine_paths.dynamic_users) {
        /* schema-user: its services already run as the user; said once */
        if (s->dynamic_user == 1) elog(stderr, "%s: dynamic_user=1 ignored, user services run as the user\n", s->name);
        s->dynamic_user = 2;
        return 0;
    }
    uid_t st;
    if (dynuser_static(s, &st)) {
        /* a carried dynamic uid must not linger in run_uid */
        if (s->dyn_uid && s->run_uid == (uid_t)s->dyn_uid) s->run_uid = st;
        s->dyn_uid = 0;
        return 0;
    }
    if (!s->dyn_uid) {
        static struct dynuser_ctx c;
        memset(&c, 0, sizeof c);
        c.self = s;
        dynuser_scan("/etc/passwd", 2, c.map);
        dynuser_scan("/etc/group", 2, c.map);
        uid_t u = dynuser_pick(s->name, dynuser_taken, &c);
        if (!u) return -1;
        s->dyn_uid = (int)u;
    }
    s->run_uid = (uid_t)s->dyn_uid;
    s->run_gid = (gid_t)s->dyn_uid;
    return 0;
}

/* a fresh parse has run_uid 0: put a carried dynamic uid back, so a stop or
 * reload runner of a running service never runs as root */
static void dynuser_apply_all(void) {
    for (int i = 0; i < svc_count; i++) {
        service_t *s = &services[i];
        if (!s->dyn_uid) continue;
        if (!s->dynamic_user) {   /* dynamic_user dropped from the config: forget the uid */
            s->dyn_uid = 0;
            continue;
        }
        s->run_uid = (uid_t)s->dyn_uid;
        s->run_gid = (gid_t)s->dyn_uid;
    }
}

static void ready_inotify_drain(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (read(ino_fd, buf, sizeof(buf)) > 0)
        ;
    if (!ready_event_due)
        ready_event_due = (int64_t)monotonic_ms() + READY_EVENT_GRACE_MS;
}

/* wds no path watch holds any more: ancestors a path has grown past, and
 * the watches of services a reload dropped */
static void path_gc(void) {
    static watch_t *live[MAX_SERVICES * MAX_WATCH];
    int n = 0;
    path_gc_due = 0;
    for (int i = 0; i < svc_count; i++)
        if (services[i].flags & SVC_PATH)
            for (int k = 0; k < services[i].watch_count; k++) live[n++] = &services[i].watch[k];
    if (path_fd >= 0) watch_gc(path_fd, live, n);
}

static void path_drain(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;
    while ((n = read(path_fd, buf, sizeof(buf))) > 0) {
        for (char *q = buf; q < buf + n;) {
            const struct inotify_event *ev = (const struct inotify_event *)q;
            q += sizeof *ev + ev->len;
            for (int i = 0; i < svc_count; i++)
                if (services[i].flags & SVC_PATH) path_event(&services[i], ev);
        }
    }
    if (path_gc_due) path_gc();
}

static void tick_service(service_t *svc,
                         const uint8_t *grp_states, int gcount) {
    uint32_t flags;
    uint8_t  prev = svc->inst.state;
    struct timespec now_mono;
    clock_gettime(CLOCK_MONOTONIC, &now_mono);

    /* Quarantine fuse check */
    if (svc->fuse && svc->inst.state != STATE_EXCISED) {
        int i;
        for (i = 0; i < MAX_DEPS && svc->dep_idx[i] >= 0; i++) {
            int di = svc->dep_idx[i];
            if (services[di].inst.state == STATE_FRICTION || services[di].inst.state == STATE_EXCISED) {
                execute_fuse_cmd(svc);
                svc->inst.state = STATE_EXCISED;
                svc->excise_reason = EXC_FUSE;
                service_log(svc, "fuse-tripped");
                if (svc->child_pid > 0) {
                    kill(svc->child_pid, SIGKILL);
                    svc_cgroup_kill_tree(svc);
                    svc->child_pid = 0;
                }
                break;
            }
        }
    }

    switch (svc->inst.state) {

        case STATE_NEW_PROCESS:
            if (svc->failsafe_pid > 0 || svc->card_pid > 0 || svc->stop_pid > 0) break;
            /* control-group: the last run's rest is still in its stop grace */
            if (svc->child_pid <= 0 && kill_sent_set(svc)) break;
            if (svc_retry_held(svc, &now_mono)) break;
            if (!svc->standby_up && service_is_standby(svc, services, svc_count)) {
                svc->flags |= SVC_NO_RESTART;
                svc->inst.state = STATE_EXCISED;
                svc->excise_reason = EXC_NONE;
                service_log(svc, "standby");
                break;
            }
            if (svc->fallback_live) {
                service_t *fb = service_fallback(svc, services, svc_count);
                if (fb && fb->child_pid > 0) break;
            }
            /* hold here silently until all deps reach a stable state */
            if (!service_deps_ready(svc, services, svc_count,
                                    grp_states, gcount)) break;
            if (svc->wait_socket[0] && !socket_accepts(svc->wait_socket)) {
                if (!svc->sock_waiting) {
                    svc->sock_waiting = 1;
                    service_log(svc, "wait-socket");
                }
                break;
            }
            svc->sock_waiting = 0;
            if (svc->cond_count) {
                char why[160];
                if (!service_conditions_met(svc, why, sizeof why)) {
                    char ev[200];
                    /* a parked socket service: no one may take the
                     * connection that woke it, so stop listening */
                    service_listen_close(svc);
                    if (svc->flags & SVC_TIMER) {
                        timer_rearm(svc);
                    } else if (svc->flags & SVC_PATH) {
                        svc->inst.state = STATE_PERFECT;
                    } else {
                        svc->inst.state = STATE_PERFECT;
                        clock_gettime(CLOCK_MONOTONIC, &svc->stable_time);
                        mark_boot_ready(svc, READY_CONDITION);
                    }
                    snprintf(ev, sizeof ev, "condition-skip %s", why);
                    service_log(svc, ev);
                    break;
                }
            }
            if (svc->listen_hold) {
                if (service_listen_open(svc) < 0) {
                    svc->listen_hold = 0;
                    svc->inst.state = STATE_EXCISED;
                    svc->excise_reason = EXC_LISTEN_FAIL;
                    service_log(svc, "listen-fail");
                    break;
                }
                svc->inst.state = STATE_PERFECT;
                clock_gettime(CLOCK_MONOTONIC, &svc->stable_time);
                mark_boot_ready(svc, READY_SOCKET);
                service_log(svc, "socket-wait");
                break;
            }
            flags = service_probe_f8(svc, services, svc_count);
            schema_step(&svc->inst, flags);
            if (svc->inst.state == STATE_FULL_TRUST && dynuser_assign(svc) != 0) {
                svc->inst.state = STATE_EXCISED;
                svc->excise_reason = EXC_EXIT_CODE;
                service_log(svc, "dynamic-uid-exhausted");
            } else if (svc->inst.state == STATE_FULL_TRUST) {
                service_log(svc, "spawn");
                eviction_forget(service_cgroup_path_for(svc));
                if (service_spawn(svc) < 0) {
                    svc->inst.state = STATE_RECOVERY;
                    service_log(svc, "spawn-fail");
                }
            } else {
                service_log(svc, "blocked");
            }
            break;

        case STATE_FULL_TRUST:
            if (svc->start_timeout_sec > 0 && svc->child_pid > 0 && !notify_paused_since && !svc->notify_ready &&
                now_mono.tv_sec - svc->spawn_time_mono.tv_sec >= svc->start_timeout_sec) {
                /* stuck in FULL_TRUST past its window — kill it.
                 * active_kill_service() reaps the child itself, so reap() won't
                 * see it; set the next state here.
                 * Non-critical: excise immediately so dependents proceed (no
                 * retry storm). Critical / no_excise: must NEVER excise (would
                 * permanently hard-block its chain) — route to RECOVERY so it
                 * retries like any other death. */
                service_log(svc, "start-timeout");
                active_kill_service(svc);
                if (svc->failsafe_cmd[0]) start_failsafe(svc);
                if (!(svc->flags & SVC_CRITICAL) && !svc->no_excise) {
                    svc->inst.state = STATE_EXCISED;
                    svc->excise_reason = EXC_START_TIMEOUT;
                    service_log(svc, "76-excised");
                } else {
                    svc->inst.state = STATE_RECOVERY;
                    service_log(svc, "timeout-recovery");
                }
                break;
            }
            if (svc->fork_state < 0) {
                fork_adopt(svc);
                if (svc->fork_state < 0 && now_mono.tv_sec >= svc->fork_wait.tv_sec) {
                    svc->fork_state = 0;
                    svc_cgroup_kill_tree(svc);
                    restart_budget_refresh(svc);
                    if (svc->failsafe_cmd[0]) start_failsafe(svc);
                    service_log(svc, "pid_file-missing");
                    if ((svc->flags & SVC_NO_RESTART) || !service_restart_wanted(svc, DEATH_TIMEOUT)) {
                        svc->inst.state = STATE_EXCISED;
                        svc->excise_reason = EXC_START_TIMEOUT;
                        service_log(svc, (svc->flags & SVC_NO_RESTART) ? "76-no-restart" : "76-restart-policy");
                    } else {
                        svc->inst.state = STATE_RECOVERY;
                    }
                }
                break;
            }
            if (!(svc->flags & SVC_ONESHOT) && svc->child_pid > 0) {
                int ready = 0;
                if (svc->pid_file[0] && svc->fork_state != 1) {
                    /* not before the launcher has exited and the daemon is adopted */
                } else if ((svc->notify || svc->ready_bus_name[0]) && svc->notify_ready) {
                    ready = svc->ready_bus_name[0] ? READY_BUS : READY_NOTIFY;
                } else if (service_verifies_limits(svc)) {
                    /* its limits are checked on READY=1: no other road to ready */
                } else if (svc->ready_path[0] && ready_path_fresh(svc)) {
                    ready = READY_PATH;
                    svc->ready_path_verified = 1;
                } else if (svc->pid_file[0]) {
                    if (svc->fork_state == 1) ready = READY_PIDFILE;
                } else if (ready_timer_due(now_mono.tv_sec - svc->spawn_time_mono.tv_sec, svc->stable_secs,
                                           svc->ready_bus_name[0] != 0, svc->cgroup_path[0] != 0,
                                           bus_reporter_seen)) {
                    ready = READY_TIMER;
                }
                if (ready) {
                    flags = service_probe_f8(svc, services, svc_count);
                    schema_step(&svc->inst, flags);
                    if (svc->inst.state == STATE_EXCISED) svc->excise_reason = EXC_PROBE;
                    if (svc->inst.state != prev) {
                        clock_gettime(CLOCK_MONOTONIC, &svc->stable_time);
                        mark_boot_ready(svc, ready);
                        service_log(svc, "promote");
                    }
                }
            }
            break;

        case STATE_RECOVERY:
            flags = service_probe_f9(svc, services, svc_count);
            schema_step(&svc->inst, flags);
            if (svc->inst.state == STATE_SETTLED) {
                /* recovery resolved → re-queue for spawn */
                svc->inst.state = STATE_NEW_PROCESS;
                svc->restart_count++;
                svc_hold_retry(svc, &now_mono);
                service_log(svc, "retry");
                if (svc->restart_sec > 0) service_log(svc, "restart-hold");
                play_card(svc);
            } else if (svc->inst.state == STATE_FRICTION) {
                service_log(svc, "friction");
            }
            break;

        case STATE_FRICTION:
            flags = service_probe_f6(svc);
            schema_step(&svc->inst, flags);
            if (svc->inst.state == STATE_RECOVERY) {
                svc->inst.state = STATE_NEW_PROCESS;
                svc->restart_count++;
                svc_hold_retry(svc, &now_mono);
                service_log(svc, "retry-deep");
                if (svc->restart_sec > 0) service_log(svc, "restart-hold");
                play_card(svc);
            } else if (svc->inst.state == STATE_EXCISED) {
                svc_cgroup_kill_tree(svc);
                engine_friction_verdict(svc);
                if (svc->fallback_live) {
                    service_t *fb = service_fallback(svc, services, svc_count);
                    if (fb) fallback_up(fb);
                }
            }
            break;

        case STATE_DORMANT: {
            struct timespec _now;
            clock_gettime(CLOCK_MONOTONIC, &_now);
            if (_now.tv_sec >= svc->dormant_until.tv_sec) {
                svc->inst.state = STATE_NEW_PROCESS;
                svc->restart_count++;
                service_log(svc, "dormant-wake");
                if (svc->fallback_live) {
                    service_t *fb = service_fallback(svc, services, svc_count);
                    if (fb) fallback_down(fb);
                }
                play_card(svc);
            }
            break;
        }

        case STATE_FUNDAMENTAL:
            if (svc->ready_path[0] && svc->child_pid > 0 && !svc->ctl_killed) {
                if (!svc->ready_path_verified) {
                    if (ready_path_fresh(svc)) {
                        svc->ready_path_verified = 1;
                    }
                }
                if (svc->ready_path_verified && !svc->ready_watched) {
                    ready_watch(svc);
                    if (svc->inst.state != STATE_FUNDAMENTAL) break;
                }
                if (svc->ready_path_verified && svc->ready_poll_hz > 0) {
                    int ticks_to_wait = 1;
                    int loop_hz = 1000000 / TICK_USEC;
                    if (loop_hz > svc->ready_poll_hz) {
                        ticks_to_wait = loop_hz / svc->ready_poll_hz;
                        if (ticks_to_wait < 1) ticks_to_wait = 1;
                    }
                    svc->ready_check_ticks++;
                    if (svc->ready_check_ticks >= ticks_to_wait) {
                        svc->ready_check_ticks = 0;
                        if (access(svc->ready_path, F_OK) != 0)
                            ready_lost(svc);
                    }
                } else if (svc->ready_path_verified && svc->ready_watched < 0 &&
                           access(svc->ready_path, F_OK) != 0) {
                    ready_lost(svc);
                }
            }
            break;

        case STATE_PERFECT:
            if ((svc->flags & SVC_PATH) && svc->path_due) {
                svc->path_due = 0;
                path_fire(svc);
            }
            if (svc->flags & SVC_TIMER) {
                struct timespec _tn;
                clock_gettime((svc->flags & SVC_TIMER_CALENDAR)
                                  ? CLOCK_REALTIME : CLOCK_MONOTONIC, &_tn);
                if (_tn.tv_sec > svc->timer_next.tv_sec ||
                    (_tn.tv_sec == svc->timer_next.tv_sec && _tn.tv_nsec >= svc->timer_next.tv_nsec)) {
                    svc->inst.state = STATE_NEW_PROCESS;
                    service_log(svc, "timer-fire");
                }
            }
            break;

        case STATE_EXCISED:
            /* nothing to do — stable, done, or dead */
            break;
    }
}

/* ── shared memory export ───────────────────────────────────────────── */

static void shm_init(void) {
    int fd;
    /* /dev/shm is shared by every user: a non-root supervisor's export is
     * its own, fresh and private. A name another user already holds can't be
     * unlinked by us, and then there is simply no export. */
    if (geteuid() != 0) {
        shm_unlink(engine_paths.shm_name);
        fd = shm_open(engine_paths.shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    } else {
        fd = shm_open(engine_paths.shm_name, O_CREAT | O_RDWR, 0644);
    }
    if (fd < 0) return;
    ftruncate(fd, (off_t)sizeof(schema_shm_t));
    shm_ptr = mmap(NULL, sizeof(schema_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm_ptr == MAP_FAILED) shm_ptr = NULL;
    else shm_ptr->magic = SCHEMA_SHM_MAGIC;
}

_Static_assert(SCHEMA_SHM_MAX >= MAX_SERVICES, "shm svc table smaller than MAX_SERVICES");
_Static_assert(SCHEMA_SHM_MAX_GROUPS >= MAX_GROUPS, "shm group table smaller than MAX_GROUPS");

/* Readers (schema-board, the shim's is-active) redraw on every seq change,
 * so seq moves only when something they can see did. system_state is written
 * straight from the signal handlers; its change still has to bump seq. */
static void shm_update(void) {
    static uint8_t last_sys;
    int i, changed = 0;
    if (!shm_ptr) return;
    for (i = 0; i < svc_count; i++) {
        shm_svc_t e;
        memset(&e, 0, sizeof e);
        memcpy(e.name, services[i].name, 64);
        e.state         = services[i].inst.state;
        e.weight        = services[i].inst.weight;
        e.child_pid     = (int32_t)services[i].child_pid;
        e.restart_count = services[i].restart_count;
        e.card_played   = (uint8_t)services[i].card_next;
        e.card_count    = (uint8_t)services[i].card_count;
        service_t *fb = services[i].fallback_live ? service_fallback(&services[i], services, svc_count) : NULL;
        e.limp = (fb && fb->standby_up ? SHM_LIMP_FALLBACK : 0)
            | (service_is_standby(&services[i], services, svc_count)
               ? (services[i].standby_up ? SHM_LIMP_STANDIN : SHM_LIMP_STANDBY) : 0);
        if (memcmp(&shm_ptr->svc[i], &e, sizeof e) != 0) {
            memcpy(&shm_ptr->svc[i], &e, sizeof e);
            changed = 1;
        }
    }
    if (shm_ptr->count != svc_count) { shm_ptr->count = svc_count; changed = 1; }
    for (i = 0; i < grp_count; i++) {
        shm_group_t g;
        memset(&g, 0, sizeof g);
        memcpy(g.name, groups[i].name, 64);
        g.state        = groups[i].state;
        g.member_count = groups[i].member_count;
        if (memcmp(&shm_ptr->groups[i], &g, sizeof g) != 0) {
            memcpy(&shm_ptr->groups[i], &g, sizeof g);
            changed = 1;
        }
    }
    if (shm_ptr->group_count != grp_count) { shm_ptr->group_count = grp_count; changed = 1; }
    if (shm_ptr->system_state != last_sys) { last_sys = shm_ptr->system_state; changed = 1; }
    if (changed) shm_ptr->seq++;
}

/* ── runtime control socket ─────────────────────────────────────────── */

/* The loop that reaps children and pets the watchdog is the one replying, and
 * the read-only group can reach this socket. A client gets 250 ms to send its
 * line, and its reply goes out in one non-blocking send: per-write timeouts
 * still let a client hold the loop by reading a little at a time. The largest
 * reply (--kv, 128 services) is about 67 KB and fits an empty socket buffer. A
 * reply that doesn't loses its terminator, which schema-ctl reports. */
#define CTL_RECV_MS  250
#define CTL_LATE_SEC 1      /* the one reply written outside ctl_poll: re-exec's */

static char   ctl_out[128 * 1024];
static size_t ctl_out_n;
static int    ctl_out_fd = -1, ctl_out_over;

static void ctl_send(int fd, const char *buf, size_t n) {
    if (fd != ctl_out_fd) {
        write(fd, buf, n);
        return;
    }
    if (ctl_out_over || n > sizeof(ctl_out) - ctl_out_n) {
        ctl_out_over = 1;
        return;
    }
    memcpy(ctl_out + ctl_out_n, buf, n);
    ctl_out_n += n;
}

static void ctl_end(int fd) {
    ctl_send(fd, ".\n", 2);
}

void ctl_writef(int fd, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    /* vsnprintf returns the untruncated length; the newline keeps the next line whole */
    if ((size_t)n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
        buf[n - 1] = '\n';
    }
    ctl_send(fd, buf, (size_t)n);
}

#define ANALYZE_BAR      40
#define ANALYZE_LATE_GAP 30.0

static int analyze_approx;

static double mono_rel(struct timespec t) {
    return (double)(t.tv_sec - init_start.tv_sec)
         + (double)(t.tv_nsec - init_start.tv_nsec) / 1e9;
}

static int ts_set(struct timespec t) { return t.tv_sec || t.tv_nsec; }

static int an_has(const service_t *s) {
    if (ts_set(s->boot_spawn) && ts_set(s->boot_ready)) return 1;
    return ts_set(s->spawn_time_mono) && ts_set(s->stable_time);
}

static double an_spawn(const service_t *s) {
    if (ts_set(s->boot_spawn) && ts_set(s->boot_ready)) return mono_rel(s->boot_spawn);
    analyze_approx = 1;
    return mono_rel(s->spawn_time_mono);
}

static double an_ready(const service_t *s) {
    if (ts_set(s->boot_spawn) && ts_set(s->boot_ready)) return mono_rel(s->boot_ready);
    analyze_approx = 1;
    return mono_rel(s->stable_time);
}

static const char *an_how(const service_t *s) {
    int how = ts_set(s->boot_ready) ? s->boot_how
            : (s->flags & SVC_ONESHOT) ? READY_EXIT : 0;
    switch (how) {
        case READY_NOTIFY: return "notify";
        case READY_BUS:    return "bus-name";
        case READY_PATH:   return "ready_path";
        case READY_TIMER:  return "timer";
        case READY_EXIT:   return "exit";
        case READY_PIDFILE: return "pid_file";
        case READY_SOCKET: return "socket";
        case READY_CONDITION: return "condition";
    }
    return "?";
}

static int an_is_timer(const service_t *s) {
    return s->timer_boot_sec > 0 || s->timer_interval_sec > 0
        || (s->flags & SVC_TIMER_CALENDAR);
}

/* The dep that became ready last: the one this service was waiting on. */
/* A group dep waits on its members, so expand it to them. Timer services
 * count as ready from the moment they are armed: never a real wait. */
struct an_dep { int svc, grp; };
#define AN_MAX_DEPS (MAX_DEPS * (1 + MAX_MEMBERS))

static int an_deps(const service_t *s, struct an_dep *out) {
    int k, m, n = 0;
    for (k = 0; k < MAX_DEPS && s->dep_name[k][0]; k++) {
        int di = s->dep_idx[k], gi = s->grp_dep_idx[k];
        if (di >= 0 && !an_is_timer(&services[di])) out[n++] = (struct an_dep){di, -1};
        if (gi >= 0)
            for (m = 0; m < groups[gi].member_count; m++) {
                int mi = groups[gi].member_idx[m];
                if (mi >= 0 && !an_is_timer(&services[mi])) out[n++] = (struct an_dep){mi, gi};
            }
    }
    return n;
}

/* The dep that became ready last: the one this service was waiting on. */
static int an_gate(const service_t *s, int *via) {
    struct an_dep d[AN_MAX_DEPS];
    int k, nd = an_deps(s, d), g = -1;
    double best = -1e18;
    if (via) *via = -1;
    for (k = 0; k < nd; k++) {
        if (!an_has(&services[d[k].svc])) continue;
        if (an_ready(&services[d[k].svc]) > best) {
            best = an_ready(&services[d[k].svc]);
            g = d[k].svc;
            if (via) *via = d[k].grp;
        }
    }
    return g;
}

/* Started well after everything it depends on was ready: activated or
 * started by hand, not part of boot. A dep that never got ready held it. */
static int an_is_late(const service_t *s) {
    struct an_dep d[AN_MAX_DEPS];
    int k, nd = an_deps(s, d), g;
    for (k = 0; k < nd; k++)
        if (!an_has(&services[d[k].svc])) return 0;
    g = an_gate(s, NULL);
    return an_spawn(s) - (g >= 0 ? an_ready(&services[g]) : 0.0) > ANALYZE_LATE_GAP;
}

static int an_gates_others(int i) {
    struct an_dep d[AN_MAX_DEPS];
    int j, k, nd;
    for (j = 0; j < svc_count; j++) {
        nd = an_deps(&services[j], d);
        for (k = 0; k < nd; k++)
            if (d[k].svc == i) return 1;
    }
    return 0;
}

static int an_gates_counted(int i, const int *counted) {
    struct an_dep d[AN_MAX_DEPS];
    int j, k, nd;
    for (j = 0; j < svc_count; j++) {
        if (!counted[j]) continue;
        nd = an_deps(&services[j], d);
        for (k = 0; k < nd; k++)
            if (d[k].svc == i) return 1;
    }
    return 0;
}

static void an_chain(int fd, int target) {
    int depth = 0, cur = target, via = -1, seen[MAX_SERVICES] = {0};
    ctl_writef(fd, "critical chain → %s   (@ spawned, + spawn→ready)\n", services[target].name);
    while (cur >= 0 && !seen[cur]) {
        const service_t *s = &services[cur];
        struct an_dep d[AN_MAX_DEPS];
        int k, nd = an_deps(s, d), gvia, g = an_gate(s, &gvia);
        char wait[48] = "", grp[80] = "";
        seen[cur] = 1;
        if (g >= 0 && an_spawn(s) - an_ready(&services[g]) >= 0.1)
            snprintf(wait, sizeof wait, "  (spawned %.3fs after dep ready)",
                     an_spawn(s) - an_ready(&services[g]));
        if (via >= 0)
            snprintf(grp, sizeof grp, " (via %s)", groups[via].name);
        ctl_writef(fd, "  %*s%s%s%s @%.3fs +%.3fs %s%s\n", depth * 2, "",
                   depth ? "└─" : "", s->name, grp, an_spawn(s),
                   an_ready(s) - an_spawn(s), an_how(s), wait);
        for (k = 0; k < nd; k++) {
            const service_t *ds = &services[d[k].svc];
            if (an_has(ds)) continue;
            if (d[k].grp >= 0)
                snprintf(grp, sizeof grp, " (via %s)", groups[d[k].grp].name);
            else
                grp[0] = '\0';
            ctl_writef(fd, "  %*s└─%s%s never ready (%s)\n", depth * 2 + 2, "",
                       ds->name, grp, state_name(ds->inst.state));
        }
        cur = g;
        via = gvia;
        depth++;
    }
}

static void ctl_analyze(int fd, const char *arg) {
    int idx[MAX_SERVICES], n = 0, i, j, target = -1, nlate = 0, ntimer = 0;
    int on_chain[MAX_SERVICES] = {0};
    int nleaf = 0, changed, counted[MAX_SERVICES] = {0};
    double horizon = 0, scale = 0, k2p = (double)init_start.tv_sec + (double)init_start.tv_nsec / 1e9;
    char late[200] = "", gating[200] = "", leaves[400] = "";

    analyze_approx = 0;
    if (*arg) {
        for (i = 0; i < svc_count; i++)
            if (strcmp(services[i].name, arg) == 0) break;
        if (i == svc_count) { ctl_writef(fd, "err: not found: %s\n", arg); return; }
        if (!an_has(&services[i])) { ctl_writef(fd, "err: %s has not reached ready\n", arg); return; }
        an_chain(fd, i);
        return;
    }

    for (i = 0; i < svc_count; i++) {
        if (an_is_timer(&services[i]) || !an_has(&services[i])) continue;
        if (an_is_late(&services[i])) {
            size_t l = strlen(late);
            if (l < sizeof late - 40)
                snprintf(late + l, sizeof late - l, "%s%s @%.0fs", nlate ? ", " : "",
                         services[i].name, an_spawn(&services[i]));
            nlate++;
            continue;
        }
        idx[n++] = i;
    }
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (an_spawn(&services[idx[j]]) < an_spawn(&services[idx[i]])) {
                int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            }
    for (i = 0; i < n; i++) counted[idx[i]] = 1;
    for (changed = 1; changed; ) {
        changed = 0;
        for (i = 0; i < n; i++) {
            const service_t *s = &services[idx[i]];
            if (counted[idx[i]] && s->boot_how == READY_TIMER && ts_set(s->boot_ready)
                && !an_gates_counted(idx[i], counted)) {
                counted[idx[i]] = 0;
                changed = 1;
            }
        }
    }
    for (i = 0; i < n; i++) {
        double rd = an_ready(&services[idx[i]]);
        if (rd > scale) scale = rd;
        if (!counted[idx[i]]) {
            size_t l = strlen(leaves);
            if (l < sizeof leaves - 40)
                snprintf(leaves + l, sizeof leaves - l, "%s%s", nleaf ? ", " : "", services[idx[i]].name);
            nleaf++;
            continue;
        }
        if (rd > horizon) { horizon = rd; target = idx[i]; }
    }
    if (target < 0) {
        nleaf = 0;
        for (i = 0; i < n; i++)
            if (an_ready(&services[idx[i]]) > horizon) { horizon = an_ready(&services[idx[i]]); target = idx[i]; }
    }
    if (target < 0) { ctl_writef(fd, "err: no service has reached ready\n"); return; }
    if (horizon <= 0) horizon = 1e-3;
    if (scale < horizon) scale = horizon;

    ctl_writef(fd, "boot: kernel %.3fs + userspace %.3fs = %.3fs   (last ready: %s)\n",
               k2p, horizon, k2p + horizon, services[target].name);
    ctl_writef(fd, "\n");
    an_chain(fd, target);
    for (i = target; i >= 0 && !on_chain[i]; i = an_gate(&services[i], NULL)) on_chain[i] = 1;

    ctl_writef(fd, "\n  %-24s %8s %8s  %-10s 0%*s%.1fs\n", "service", "spawn", "cost", "ready-by",
               ANALYZE_BAR - 4, "", scale);
    for (i = 0; i < n; i++) {
        const service_t *s = &services[idx[i]];
        double sp = an_spawn(s), rd = an_ready(s);
        int a = (int)(sp / scale * ANALYZE_BAR), b = (int)(rd / scale * ANALYZE_BAR + 0.999);
        char bar[ANALYZE_BAR * 3 + 1], *p = bar;
        const char *fill = s->boot_how == READY_TIMER && ts_set(s->boot_ready) ? "░" : "█";
        int c;
        if (b <= a) b = a + 1;
        if (b > ANALYZE_BAR) b = ANALYZE_BAR;
        for (c = 0; c < a && c < ANALYZE_BAR; c++) *p++ = ' ';
        for (; c < b; c++) { memcpy(p, fill, 3); p += 3; }
        *p = '\0';
        ctl_writef(fd, "%c %-24.24s %7.3fs %7.3fs  %-10s |%s\n", on_chain[idx[i]] ? '*' : ' ',
                   s->name, sp, rd - sp, an_how(s), bar);
        if (s->boot_how == READY_TIMER && ts_set(s->boot_ready)) {
            if (an_gates_others(idx[i])) {
                size_t l = strlen(gating);
                if (l < sizeof gating - 30)
                    snprintf(gating + l, sizeof gating - l, "%s%s", ntimer ? ", " : "", s->name);
                ntimer++;
            }
        }
    }
    ctl_writef(fd, "\n* = on the critical chain   █ readiness signalled   ░ promoted by stable timer\n");
    if (ntimer)
        ctl_writef(fd, "timer-gated with dependents (%d): %s — notify=1 or ready_path= would release them sooner\n",
                   ntimer, gating);
    if (nleaf)
        ctl_writef(fd, "not counted in boot (%d, timer-promoted, nothing counted waits on them): %s\n",
                   nleaf, leaves);
    if (nlate)
        ctl_writef(fd, "started after boot (%d): %s\n", nlate, late);
    if (analyze_approx)
        ctl_writef(fd, "note: some rows use the current run, not boot (PID 1 re-exec'd from a build without boot stamps)\n");
}

/* Why the service's last run ended: "SIGSEGV (core dumped)", "exit 1", "-". */
static const char *last_exit_str(const service_t *s) {
    static char b[48];
    if (s->term_signal) {
        const char *ab = sig_abbrev(s->term_signal);
        snprintf(b, sizeof b, "SIG%s%s", ab ? ab : "?", s->core_dumped ? " (core dumped)" : "");
    } else if (s->has_exited && s->exit_status >= 0) {
        snprintf(b, sizeof b, "exit %d", s->exit_status);
    } else {
        snprintf(b, sizeof b, "-");
    }
    return b;
}

/* What a bare reset retries. EXC_NONE is on purpose (stop, standby,
 * fallback-down) and EXC_BIND is waiting for its bound: neither failed. A stop
 * holds SVC_NO_RESTART beyond the config: start_timeout_sec can overtake a slow
 * one and record a failure reason, and it is still a stop. */
int svc_failed(const service_t *s) {
    if ((s->flags & SVC_NO_RESTART) && !s->no_restart_cfg) return 0;
    return s->inst.state == STATE_DORMANT ||
           (s->inst.state == STATE_EXCISED && s->excise_reason != EXC_NONE && s->excise_reason != EXC_BIND);
}

/* JSON string body: name= is free text from the .svc, so quotes, backslashes,
 * control bytes AND bytes past ASCII must not break the "machine-parseable"
 * promise — a raw 0xE9 is invalid UTF-8 and strict parsers refuse the whole
 * reply. Every byte >= 0x7F becomes \u00XX (a multibyte UTF-8 name comes out
 * byte-escaped rather than pretty, but the JSON always parses). out needs
 * 6*strlen(s)+1; anything past that is dropped, never half-escaped. */
static const char *json_esc(const char *s, char *out, size_t outsz) {
    size_t o = 0;
    for (; *s && o + 7 <= outsz; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20 || c >= 0x7f) o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = '\0';
    return out;
}

/* One cgroup file of a live service: the first line, or the value after KEY
 * in a flat-keyed file (memory.events, cpu.stat). "" if absent. Read on
 * request only; the loop never touches these. */
static const char *cg_peek(const service_t *s, const char *file, const char *key, char *out, size_t n) {
    char p[400], l[128];
    FILE *f;
    size_t kl = key ? strlen(key) : 0;
    out[0] = '\0';
    if (!s->cgroup_path[0] || s->child_pid <= 0) return out;
    snprintf(p, sizeof p, "%s/%s", s->cgroup_path, file);
    if (!(f = fopen(p, "r"))) return out;
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = '\0';
        if (!key) { snprintf(out, n, "%s", l); break; }
        if (!strncmp(l, key, kl) && l[kl] == ' ') { snprintf(out, n, "%s", l + kl + 1); break; }
    }
    fclose(f);
    return out;
}

/* bytes as MiB, "max" and "" passed through */
static const char *cg_mib(const char *v, char *out, size_t n) {
    if (!*v || strspn(v, "0123456789") != strlen(v)) snprintf(out, n, "%s", *v ? v : "-");
    else snprintf(out, n, "%.1fM", strtoull(v, NULL, 10) / 1048576.0);
    return out;
}

static void ctl_cmd(int fd, char *line) {
    int i, extra;
    line[strcspn(line, "\r\n")] = '\0';

    if (strncmp(line, "status", 6) == 0) {
        char *opt = line + 6;
        while (*opt == ' ') opt++;
        if (strcmp(opt, "--json") == 0) {
            char jn[6 * sizeof(services[0].name) + 1], jb[6 * sizeof(services[0].bind_name) + 1];
            ctl_writef(fd, "{\n  \"services_count\": %d,\n  \"groups_count\": %d,\n  \"services\": [\n", svc_count, grp_count);
            for (i = 0; i < svc_count; i++) {
                ctl_writef(fd, "    {\n      \"name\": \"%s\",\n      \"pid\": %d,\n      \"state\": \"%s\",\n      \"restarts\": %d,\n      \"circuit_broken\": %s,\n      \"excise_reason\": \"%s\",\n      \"bind\": \"%s\",\n      \"bind_held\": %s\n    }%s\n",
                    json_esc(services[i].name, jn, sizeof jn), (int)services[i].child_pid,
                    state_name(services[i].inst.state), services[i].restart_count,
                    services[i].circuit_broken ? "true" : "false",
                    excise_reason_name(services[i].excise_reason),
                    json_esc(services[i].bind_name, jb, sizeof jb),
                    services[i].bind_held ? "true" : "false",
                    (i < svc_count - 1) ? "," : "");
            }
            ctl_writef(fd, "  ]\n}\n");
        } else if (strcmp(opt, "--kv") == 0) {
            ctl_writef(fd, "services_count=%d\ngroups_count=%d\nbooted_version=%s\n", svc_count, grp_count,
                       booted_version);
            /* left out when not known (re-exec'd from an image that did not record it) */
            if (booted_image[0]) ctl_writef(fd, "booted_image=%s\n", booted_image);
            for (i = 0; i < svc_count; i++) {
                ctl_writef(fd, "service.%s.pid=%d\n", services[i].name, (int)services[i].child_pid);
                ctl_writef(fd, "service.%s.state=%s\n", services[i].name, state_name(services[i].inst.state));
                ctl_writef(fd, "service.%s.restarts=%d\n", services[i].name, services[i].restart_count);
                ctl_writef(fd, "service.%s.last_exit=%s\n", services[i].name, last_exit_str(&services[i]));
                ctl_writef(fd, "service.%s.circuit_broken=%d\n", services[i].name, services[i].circuit_broken);
                ctl_writef(fd, "service.%s.excise_reason=%s\n", services[i].name,
                    excise_reason_name(services[i].excise_reason));
                /* the verify settle wait judges failure by svc_failed(), not its own copy */
                ctl_writef(fd, "service.%s.failed=%d\n", services[i].name, svc_failed(&services[i]));
                ctl_writef(fd, "service.%s.critical=%d\n", services[i].name, !!(services[i].flags & SVC_CRITICAL));
                if (services[i].bind_name[0]) {
                    ctl_writef(fd, "service.%s.bind=%s\n", services[i].name, services[i].bind_name);
                    ctl_writef(fd, "service.%s.bind_held=%d\n", services[i].name, services[i].bind_held);
                }
                if (svc_held_on_socket(&services[i]))
                    ctl_writef(fd, "service.%s.waiting=%s\n", services[i].name, services[i].wait_socket);
            }
        } else if (*opt) {
            for (i = 0; i < svc_count && strcmp(services[i].name, opt) != 0; i++) ;
            if (i == svc_count) {
                ctl_writef(fd, "err: not found: %s\n", opt);
            } else {
                const service_t *s = &services[i];
                ctl_writef(fd, "%s  pid=%d  state=%s  restarts=%d\n", s->name,
                    (int)s->child_pid, state_name(s->inst.state), s->restart_count);
                ctl_writef(fd, "last exit: %s\n", last_exit_str(s));
                if (s->cgroup_path[0] && s->child_pid > 0) {
                    char v[128], a[32], b[32], c[32], d[32], e[32];
                    cg_mib(cg_peek(s, "memory.current", NULL, v, sizeof v), a, sizeof a);
                    cg_mib(cg_peek(s, "memory.peak", NULL, v, sizeof v), b, sizeof b);
                    cg_mib(cg_peek(s, "memory.swap.current", NULL, v, sizeof v), c, sizeof c);
                    cg_mib(cg_peek(s, "memory.low", NULL, v, sizeof v), d, sizeof d);
                    cg_mib(cg_peek(s, "memory.swap.max", NULL, v, sizeof v), e, sizeof e);
                    ctl_writef(fd, "memory: current=%s peak=%s swap=%s low=%s swap.max=%s oom_kill=%s\n",
                               a, b, c, d, e, *cg_peek(s, "memory.events", "oom_kill", v, sizeof v) ? v : "-");
                    if (*cg_peek(s, "cpu.stat", "usage_usec", v, sizeof v))
                        ctl_writef(fd, "cpu: %.2fs\n", strtoull(v, NULL, 10) / 1e6);
                }
                if (s->circuit_broken)
                    ctl_writef(fd, "circuit_broken=1: crash-loop breaker excised it; schema-ctl reset %s\n", s->name);
                else if (s->excise_reason)
                    ctl_writef(fd, "excise_reason=%s\n", excise_reason_name(s->excise_reason));
                if (s->bind_name[0])
                    ctl_writef(fd, "bound to %s%s\n", s->bind_name,
                               s->bind_held ? " (held)" : "");
                ctl_writef(fd, "hardening default: %s\n", service_hardening_default() ? "ON" : "off");
                ctl_writef(fd, "  %-15s %-5s %s\n", "no_new_privs",
                    (s->flags & SVC_NO_NEW_PRIVS) ? "1" : "0", svc_hard_source(s, HARD_NNP));
                ctl_writef(fd, "  %-15s %-5d %s\n", "private_tmp", s->ns_private_tmp, svc_hard_source(s, HARD_PT));
                ctl_writef(fd, "  %-15s %-5s %s\n", "protect_system",
                    svc_protect_system_name(s->ns_protect_system), svc_hard_source(s, HARD_PS));
                ctl_writef(fd, "  %-15s %-5d %s\n", "protect_home", s->ns_protect_home, svc_hard_source(s, HARD_PH));
                ctl_writef(fd, "  %-15s %-5s %s\n", "syscall_deny",
                    svc_sc_deny_label(s), svc_hard_source(s, HARD_SD));
                for (int k = 0; k < NS_K_COUNT; k++)
                    if (s->ns_kernel >> k & 1) ctl_writef(fd, "  %s=1\n", ns_kernel_names[k]);
                if (s->umask_val >= 0) ctl_writef(fd, "  umask=%04o\n", s->umask_val);
                if (s->sc.mode)
                    ctl_writef(fd, "  syscall_filter: %s, %d syscalls, blocked -> %s\n",
                               s->sc.mode == SC_ALLOW ? "allowlist" : "denylist", sc_count(&s->sc),
                               s->sc.err == SC_KILL ? "kill" : s->sc.err ? strerror(s->sc.err) : strerror(EPERM));
                if (sc_active(&s->sc))
                    ctl_writef(fd, "  seccomp: native-arch%s%s%s%s\n",
                               s->sc.suid ? " restrict_suid_sgid" : "", s->sc.persona ? " lock_personality" : "",
                               s->sc.rt ? " restrict_realtime" : "", s->sc.ns ? " restrict_namespaces" : "");
                if (s->dyn_uid) ctl_writef(fd, "  dynamic uid %d\n", s->dyn_uid);
                for (int k = 0; k < s->supp_count; k++)
                    ctl_writef(fd, "  supplementary_group=%s\n", s->supp_groups[k]);
                for (int k = 0; k < s->watch_count; k++)
                    ctl_writef(fd, "  watch_path=%s:%s\n", watch_kind_name(s->watch[k].kind), s->watch[k].path);
                if ((s->flags & SVC_TIMER) && s->inst.state == STATE_PERFECT) {
                    time_t t = s->timer_next.tv_sec;
                    char tb[32];
                    if (!(s->flags & SVC_TIMER_CALENDAR)) {   /* a CLOCK_MONOTONIC deadline */
                        struct timespec m;
                        clock_gettime(CLOCK_MONOTONIC, &m);
                        t = time(NULL) + (s->timer_next.tv_sec - m.tv_sec);
                    }
                    strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", localtime(&t));
                    ctl_writef(fd, "  next fire: %s\n", tb);
                }
                if (s->path_last_fire) {
                    char tb[32];
                    strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", localtime(&s->path_last_fire));
                    ctl_writef(fd, "  last fire: %s\n", tb);
                }
                {
                    static const char *const ioc[] = { "none", "realtime", "best-effort", "idle" };
                    static const char *const pol[] = { "other", "fifo", "rr", "batch", "?", "idle" };
                    if (s->nice_set) ctl_writef(fd, "  nice=%d\n", s->nice_val);
                    if (s->io_class >= 0) ctl_writef(fd, "  io_class=%s\n", ioc[s->io_class & 3]);
                    if (s->io_prio >= 0) ctl_writef(fd, "  io_priority=%d\n", s->io_prio);
                    if (s->sched_policy >= 0 && s->sched_policy < 6)
                        ctl_writef(fd, "  sched_policy=%s\n", pol[s->sched_policy]);
                    if (s->sched_prio) ctl_writef(fd, "  sched_priority=%d\n", s->sched_prio);
                }
                for (int k = 0; k < s->rw_path_count; k++)
                    ctl_writef(fd, "  %-15s %s\n", "rw_path", s->rw_path[k]);
                for (int k = 0; k < s->landlock_count; k++)
                    ctl_writef(fd, "  %-15s %s\n", ((s->landlock_rw >> k) & 1) ? "landlock_rw" : "landlock_ro",
                        s->landlock[k]);
                for (int t = 0; t < 2; t++) {
                    char ports[MAX_LANDLOCK * 7 + 8] = "none";
                    size_t o = 0;
                    if (s->landlock_tcp_n[t] < 0) continue;
                    for (int k = 0; k < s->landlock_tcp_n[t]; k++) {
                        int w = snprintf(ports + o, sizeof ports - o, "%s%u", k ? "," : "", s->landlock_tcp[t][k]);
                        if (w < 0 || (size_t)w >= sizeof ports - o) break;
                        o += (size_t)w;
                    }
                    ctl_writef(fd, "  %-15s %s\n", t ? "connect_tcp" : "bind_tcp", ports);
                }
            }
        } else {
            ctl_writef(fd, "services: %d  groups: %d\n", svc_count, grp_count);
            for (i = 0; i < svc_count; i++) {
                char deck[96] = "";
                if (services[i].card_count)
                    snprintf(deck, sizeof deck, "  cards=%d/%d", services[i].card_next, services[i].card_count);
                int sk = services[i].fdstore_max > 0 ? fdstore_count(services[i].name) : 0;
                if (sk > 0)
                    snprintf(deck + strlen(deck), sizeof deck - strlen(deck), "  fds=%d", sk);
                if (services[i].circuit_broken)
                    snprintf(deck + strlen(deck), sizeof deck - strlen(deck), "  circuit_broken=1");
                {
                    char v[128], m[32];
                    if (*cg_peek(&services[i], "memory.current", NULL, v, sizeof v))
                        snprintf(deck + strlen(deck), sizeof deck - strlen(deck), "  mem=%s", cg_mib(v, m, sizeof m));
                }
                ctl_writef(fd, "  %-24s  pid=%-6d  state=%-14s  restarts=%d%s%s%s%s%s\n",
                    services[i].name, (int)services[i].child_pid,
                    state_name(services[i].inst.state), services[i].restart_count, deck,
                    services[i].term_signal ? "  last=" : "",
                    services[i].term_signal ? last_exit_str(&services[i]) : "",
                    svc_held_on_socket(&services[i]) ? "  waiting=" : "",
                    svc_held_on_socket(&services[i]) ? services[i].wait_socket : "");
            }
        }

    } else if (strncmp(line, "pet ", 4) == 0) {
        const char *name = line + 4;
        for (i = 0; i < svc_count; i++) {
            if (strcmp(services[i].name, name) != 0) continue;
            clock_gettime(CLOCK_MONOTONIC, &services[i].last_pet);
            ctl_writef(fd, "ok\n");
            ctl_end(fd);
            return;
        }
        ctl_writef(fd, "err: not found: %s\n", name);

    } else if (strcmp(line, "list") == 0) {
        for (i = 0; i < svc_count; i++)
            ctl_writef(fd, "%s\n", services[i].name);

    } else if (strncmp(line, "start ", 6) == 0 || strncmp(line, "up ", 3) == 0) {
        const char *name = (strncmp(line, "start ", 6) == 0) ? line + 6 : line + 3;
        for (i = 0; i < svc_count; i++) {
            if (strcmp(services[i].name, name) != 0) continue;
            if (services[i].inst.state == STATE_EXCISED ||
                services[i].inst.state == STATE_PERFECT) {
                restart_policy_restore(&services[i]);
                services[i].listen_hold  = 0;
                services[i].inst.state   = STATE_NEW_PROCESS;
                services[i].restart_count = 0;
                ctl_rearm(&services[i], 1);
                ctl_writef(fd, "ok: %s queued\n", name);
            } else if (services[i].inst.state == STATE_NEW_PROCESS && held_now(&services[i])) {
                ctl_rearm(&services[i], 1);
                ctl_writef(fd, "ok: %s restart_sec hold cleared\n", name);
            } else {
                ctl_writef(fd, "err: %s is %s\n",
                    name, state_name(services[i].inst.state));
            }
            ctl_end(fd);
            return;
        }
        ctl_writef(fd, "err: not found: %s\n", name);

    } else if (strncmp(line, "stop ", 5) == 0 || strncmp(line, "down ", 5) == 0) {
        const char *name = line + 5;
        for (i = 0; i < svc_count; i++) {
            if (strcmp(services[i].name, name) != 0) continue;
            services[i].flags |= SVC_NO_RESTART;
            services[i].path_stop = 1;
            services[i].restart_at.tv_sec = services[i].restart_at.tv_nsec = 0;
            /* stopped on purpose: its bound's return must not start it */
            services[i].bind_held = 0;
            services[i].bind_died_at.tv_sec = services[i].bind_died_at.tv_nsec = 0;
            if (services[i].child_pid > 0) {
                services[i].ctl_killed = 1;
                stop_main(&services[i]);
                char sn[16];
                sig_text(kill_sig(&services[i]), sn, sizeof sn);
                if (services[i].stop_pid > 0)
                    ctl_writef(fd, "ok: exec_stop → %s (pid %d)\n", name, (int)services[i].child_pid);
                else
                    ctl_writef(fd, "ok: %s → %s (pid %d)\n", sn, name, (int)services[i].child_pid);
            } else {
                ctl_wait_abort(&services[i]);
                services[i].inst.state = STATE_EXCISED;
                services[i].excise_reason = EXC_NONE;
                ctl_writef(fd, "ok: %s stopped (was not running)\n", name);
            }
            ctl_end(fd);
            return;
        }
        ctl_writef(fd, "err: not found: %s\n", name);

    } else if (strncmp(line, "restart ", 8) == 0) {
        const char *name = line + 8;
        for (i = 0; i < svc_count; i++) {
            if (strcmp(services[i].name, name) != 0) continue;
            restart_policy_restore(&services[i]);
            services[i].listen_hold = 0;
            if (services[i].child_pid > 0) {
                services[i].ctl_killed = 1;
                stop_main(&services[i]);
                char sn[16];
                sig_text(kill_sig(&services[i]), sn, sizeof sn);
                ctl_writef(fd, "ok: %s → %s — recovery arc will respawn\n", sn, name);
            } else {
                ctl_wait_abort(&services[i]);
                services[i].inst.state    = STATE_NEW_PROCESS;
                services[i].restart_count = 0;
                ctl_rearm(&services[i], 1);
                ctl_writef(fd, "ok: %s requeued\n", name);
            }
            ctl_end(fd);
            return;
        }
        ctl_writef(fd, "err: not found: %s\n", name);

    } else if (strncmp(line, "reset", 5) == 0) {
        const char *name = line + 5;
        while (*name == ' ') name++;
        if (*name == '\0') {
            int requeued = 0;
            for (i = 0; i < svc_count; i++) {
                service_t *s = &services[i];
                int failed = svc_failed(s);
                s->restart_count = 0;
                s->dormant_count = 0;
                s->card_next = 0;
                breaker_clear(s);
                if (failed) {
                    ctl_rearm(s, 0);
                    restart_policy_restore(s);
                    s->inst.state = STATE_NEW_PROCESS;
                    requeued++;
                } else if (s->inst.state == STATE_NEW_PROCESS) {
                    s->restart_at.tv_sec = s->restart_at.tv_nsec = 0;
                }
            }
            ctl_writef(fd, "ok: counters cleared, %d failed requeued\n", requeued);
        } else {
            for (i = 0; i < svc_count; i++) {
                if (strcmp(services[i].name, name) != 0) continue;
                services[i].restart_count = 0;
                services[i].dormant_count = 0;
                ctl_rearm(&services[i], 0);
                restart_policy_restore(&services[i]);
                if (services[i].inst.state == STATE_EXCISED ||
                    services[i].inst.state == STATE_DORMANT ||
                    services[i].inst.state == STATE_PERFECT) {
                    services[i].inst.state = STATE_NEW_PROCESS;
                    ctl_writef(fd, "ok: reset and queued %s\n", name);
                } else {
                    ctl_writef(fd, "ok: reset restart count for %s (current state: %s)\n",
                               name, state_name(services[i].inst.state));
                }
                ctl_end(fd);
                return;
            }
            ctl_writef(fd, "err: not found: %s\n", name);
        }
        ctl_end(fd);
        return;


    } else if (strncmp(line, "add ", 4) == 0) {
        const char *path = line + 4;
        int j, k;
        if (svc_count >= MAX_SERVICES) {
            ctl_writef(fd, "err: MAX_SERVICES (%d) reached\n", MAX_SERVICES);
            ctl_end(fd);
            return;
        }
        if (service_load_one(path, &services[svc_count]) < 0) {
            ctl_writef(fd, "err: failed to load %s\n", path);
            ctl_end(fd);
            return;
        }
        for (j = 0; j < svc_count; j++) {
            if (strcmp(services[j].name, services[svc_count].name) == 0) {
                ctl_writef(fd, "err: '%s' already loaded\n", services[svc_count].name);
                for (k = 1; k < MAX_ARGV; k++)
                    if (services[svc_count].argv[k]) { free(services[svc_count].argv[k]); services[svc_count].argv[k] = NULL; }
                service_free_landlock(&services[svc_count]);
                ctl_end(fd);
                return;
            }
        }
        if (validate_and_resolve(services, svc_count + 1, groups, grp_count) > 0) {
            ctl_writef(fd, "err: '%s' introduces a dependency cycle — rejected\n", services[svc_count].name);
            for (k = 1; k < MAX_ARGV; k++)
                if (services[svc_count].argv[k]) { free(services[svc_count].argv[k]); services[svc_count].argv[k] = NULL; }
            service_free_landlock(&services[svc_count]);
            memset(&services[svc_count], 0, sizeof(service_t));
            validate_and_resolve(services, svc_count, groups, grp_count);
            ctl_end(fd);
            return;
        }
        /* Arm timers added at runtime, mirroring the boot/reload arm sites.
         * Without this the timer keeps timer_next=0 (a past CLOCK_REALTIME
         * instant) and fires once immediately on the next tick. */
        if (services[svc_count].flags & SVC_TIMER) timer_arm(&services[svc_count]);
        if (services[svc_count].flags & SVC_PATH) path_arm(&services[svc_count]);
        ctl_writef(fd, "ok: %s queued\n", services[svc_count].name);
        svc_count++;
        ctl_end(fd);
        return;

    } else if (strncmp(line, "reload ", 7) == 0 && line[7 + strspn(line + 7, " ")] &&
               strcmp(line + 7 + strspn(line + 7, " "), "--evict") != 0) {
        const char *name = line + 7;
        while (*name == ' ') name++;
        for (i = 0; i < svc_count; i++) {
            service_t *s = &services[i];
            if (strcmp(s->name, name) != 0) continue;
            if (s->child_pid <= 0)
                ctl_writef(fd, "err: %s is not running\n", name);
            else if (!s->exec_reload_count && !s->reload_signal)
                ctl_writef(fd, "err: cannot-reload: %s has no exec_reload= or reload_signal=\n", name);
            else if (s->reload_pid > 0)
                ctl_writef(fd, "err: %s is already reloading\n", name);
            else if (s->exec_reload_count) {
                pid_t rp = service_reload_spawn(s, s->child_pid);
                if (rp < 0) {
                    ctl_writef(fd, "err: %s: fork: %s\n", name, strerror(errno));
                } else {
                    s->reload_pid = rp;
                    clock_gettime(CLOCK_MONOTONIC, &s->reload_start);
                    service_log(s, "reload-cmd");
                    ctl_writef(fd, "ok: exec_reload → %s (pid %d)\n", name, (int)s->child_pid);
                }
            } else if (kill(s->child_pid, s->reload_signal) != 0) {
                ctl_writef(fd, "err: %s: signal: %s\n", name, strerror(errno));
            } else {
                const char *ab = sig_abbrev(s->reload_signal);
                service_log(s, "reload-signal");
                if (ab) ctl_writef(fd, "ok: SIG%s → %s (pid %d)\n", ab, name, (int)s->child_pid);
                else ctl_writef(fd, "ok: signal %d → %s (pid %d)\n", s->reload_signal, name, (int)s->child_pid);
            }
            ctl_end(fd);
            return;
        }
        ctl_writef(fd, "err: not found: %s\n", name);

    } else if (strncmp(line, "reload", 6) == 0) {
        char *opt = line + 6;
        while (*opt == ' ') opt++;
        int evict = (strcmp(opt, "--evict") == 0);
        char rerr[256] = "";
        if (handle_reload(evict, rerr, sizeof(rerr)) == 0)
            ctl_writef(fd, "ok: reload %s\n", evict ? "(evict mode)" : "(graceful)");
        else
            ctl_writef(fd, "err: reload rejected — %s\n", rerr[0] ? rerr : "see console");

    } else if (strcmp(line, "timing") == 0) {
        struct trow { const char *name; double cost; double ready; };
        struct trow rows[MAX_SERVICES];
        int nrows = 0, j;
        double k2p = (double)init_start.tv_sec + (double)init_start.tv_nsec / 1e9;
        ctl_writef(fd, "kernel → PID 1:            %.3fs\n", k2p);
        for (i = 0; i < svc_count; i++) {
            if (services[i].stable_time.tv_sec == 0) continue;
            rows[nrows].name  = services[i].name;
            rows[nrows].ready = (double)(services[i].stable_time.tv_sec - init_start.tv_sec)
                              + (double)(services[i].stable_time.tv_nsec - init_start.tv_nsec) / 1e9;
            /* cost = self-time from spawn to ready; the readiness column above is
             * only a wall-clock instant. A zero-initialized timespec {0,0} means
             * never observed spawning (adopted/oneshot with no fork) -> unknown,
             * sinks to bottom. Test tv_nsec too: CLOCK_MONOTONIC is uptime, so a
             * service spawned in the first second has tv_sec==0 legitimately. */
            if (services[i].spawn_time_mono.tv_sec == 0
                && services[i].spawn_time_mono.tv_nsec == 0)
                rows[nrows].cost = -1.0;
            else
                rows[nrows].cost = (double)(services[i].stable_time.tv_sec - services[i].spawn_time_mono.tv_sec)
                                 + (double)(services[i].stable_time.tv_nsec - services[i].spawn_time_mono.tv_nsec) / 1e9;
            nrows++;
        }
        /* sort by cost desc: the long pole on the critical path lands first */
        for (i = 0; i < nrows; i++)
            for (j = i + 1; j < nrows; j++)
                if (rows[j].cost > rows[i].cost) {
                    struct trow t = rows[i]; rows[i] = rows[j]; rows[j] = t;
                }
        ctl_writef(fd, "  %-24s %8s  %8s\n", "service", "cost", "ready@");
        for (i = 0; i < nrows; i++) {
            if (rows[i].cost < 0)
                ctl_writef(fd, "  %-24s %8s  %7.3fs\n", rows[i].name, "-", rows[i].ready);
            else
                ctl_writef(fd, "  %-24s %7.3fs  %7.3fs\n", rows[i].name, rows[i].cost, rows[i].ready);
        }

    } else if (strcmp(line, "analyze") == 0 || strncmp(line, "analyze ", 8) == 0) {
        const char *arg = line + 7;
        while (*arg == ' ') arg++;
        ctl_analyze(fd, arg);

    } else if ((extra = engine_hooks.ctl_extra(fd, line)) != 0) {
        if (extra == 2)
            return;                     /* the reply comes later */

    } else {
        ctl_writef(fd, "err: unknown: %s\n", line);
    }
    ctl_end(fd);
}

void set_nonblock(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void ctl_init(void) {
    struct sockaddr_un addr;
    unlink(engine_paths.ctl_sock);
    ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (ctl_fd < 0) return;
    set_nonblock(ctl_fd);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, engine_paths.ctl_sock, sizeof(addr.sun_path) - 1);
    if (bind(ctl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (!engine_paths.ctl_fallback) { close(ctl_fd); ctl_fd = -1; return; }
        mkdir("./run", 0755);
        unlink(engine_paths.ctl_fallback);
        strncpy(addr.sun_path, engine_paths.ctl_fallback, sizeof(addr.sun_path) - 1);
        if (bind(ctl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(ctl_fd); ctl_fd = -1; return;
        }
    }
    /* If a "schema" group exists, grant it read-only access (status/list/timing)
     * by opening the socket to root:schema 0660. Writes are still gated per-call
     * by peer uid in ctl_poll(). No group → root-only 0600 as before. A non-root
     * supervisor can't chown to root, so its socket is owner-only. */
    {
        struct group *g = geteuid() == 0 ? getgrnam("schema") : NULL;
        if (g && chown(addr.sun_path, 0, g->gr_gid) == 0)
            chmod(addr.sun_path, 0660);
        else
            chmod(addr.sun_path, 0600);
    }
    listen(ctl_fd, 4);
}

/* Read-only verbs any connected uid may run. Everything else needs uid 0.
 * Match the first whitespace-delimited token exactly so e.g. "statusfoo" is
 * not treated as "status" (anchored allowlist); "status --json" still passes. */
static int ctl_is_readonly(const char *line) {
    size_t n = strcspn(line, " \t");
    return (n == 6 && memcmp(line, "status", 6) == 0)
        || (n == 4 && memcmp(line, "list", 4) == 0)
        || (n == 6 && memcmp(line, "timing", 6) == 0)
        || (n == 7 && memcmp(line, "analyze", 7) == 0);
}

/* Drain the sd_notify socket. A message counts only for a notify=1 service
 * whose cgroup (or main pid, when cgroups are unavailable) holds the sender. */
static void notify_poll(int max_batch) {
    char buf[NOTIFY_MSG_MAX];
    /* The socket is world-writable, so a sender must never be able to hold
     * PID 1 here: bounded batch per wakeup, and a hard recv error ends it. */
    for (int batch = 0; batch < max_batch; batch++) {
        fdclose_sender_pid = 0;
        fdclose_sender_uid = (uid_t)-1;
        /* every received fd needs a closer or a parking place: stop while
         * a full message might not fit */
        if (fdclose_full()) {
            notify_pause_track();
            return;
        }
        pid_t pid = 0;
        uid_t uid = (uid_t)-1;
        int rfds[NOTIFY_MAX_FDS], nrfds = 0;
        ssize_t n = notify_recv(notify_fd, buf, sizeof buf, &pid, &uid, rfds, &nrfds);
        if (n == 0) return;
        if (n == -2) return;
        if (n < 0) continue;
        fdclose_sender_pid = pid;
        fdclose_sender_uid = uid;
        struct notify_msg m;
        int acted = notify_parse(buf, (size_t)n, &m);
        if (!acted) {
            fd_close_detached(rfds, nrfds);
            continue;
        }
        char cgtext[NOTIFY_CGROUP_TEXT_MAX];
        int have_cg = -1;                           /* read lazily, once per message */
        if (m.busname[0]) {
            /* "this name now has an owner": only the broker may say so, and it
             * counts for the service whose cgroup holds the owning pid. */
            fd_close_detached(rfds, nrfds);
            if (m.bus_owner_pid <= 0 || !notify_sender_is_broker(pid, uid)) continue;
            bus_reporter_seen = 1;
            for (int i = 0; i < svc_count; i++) {
                service_t *svc = &services[i];
                if (!svc->ready_bus_name[0] || svc->child_pid <= 0 || svc->notify_ready) continue;
                if (strcmp(svc->ready_bus_name, m.busname) != 0) continue;
                if (svc->child_pid != m.bus_owner_pid) {
                    if (!svc->cgroup_path[0]) continue;
                    if (have_cg < 0) have_cg = notify_read_cgroup(m.bus_owner_pid, cgtext, sizeof cgtext);
                    if (!have_cg || !notify_cgroup_text_matches(cgtext, svc->cgroup_path)) continue;
                }
                svc->notify_ready = 1;
                service_log(svc, "bus-name-ready");
            }
            continue;
        }
        for (int i = 0; i < svc_count; i++) {
            service_t *svc = &services[i];
            int stores = svc->fdstore_max > 0 && (m.fdstore || m.fdstore_remove) && svc->child_pid == pid;
            if (!(svc->notify || stores) || (svc->child_pid <= 0 && !svc->main_wait)) continue;
            if (svc->child_pid != pid) {
                if (!svc->cgroup_path[0]) continue;
                if (have_cg < 0) have_cg = notify_read_cgroup(pid, cgtext, sizeof cgtext);
                if (!have_cg || !notify_cgroup_text_matches(cgtext, svc->cgroup_path)) continue;
            }
            if (stores && m.fdstore) {
                int kept = 0;
                for (int k = 0; k < nrfds; k++)
                    if (fdstore_add(svc->name, rfds[k], m.fdname, svc->fdstore_max) < 0) rfds[kept++] = rfds[k];
                nrfds = kept;
                if (kept) service_log(svc, "fdstore-refused");
            }
            if (stores && m.fdstore_remove)
                fdstore_remove(svc->name, m.fdname);
            if (!svc->notify) break;
            const char *mp_why = m.mainpid && (svc->flags & SVC_MAIN_PID_NOTIFY) ? mainpid_why(svc, m.mainpid) : NULL;
            if (mp_why) mainpid_rejected(svc, m.mainpid, mp_why);
            else if (m.mainpid && (svc->flags & SVC_MAIN_PID_NOTIFY)) {
                int adopted = svc->main_pid && svc->child_pid == svc->main_pid;
                if (svc->main_wait || adopted) {
                    if (pid_parent(m.mainpid) == getpid()) main_adopt(svc, m.mainpid);
                    else mainpid_rejected(svc, m.mainpid, "not-ours");
                } else if (m.mainpid != svc->child_pid) {
                    svc->main_pid = m.mainpid;   /* launcher still running: adopt at its exit */
                }
            }
            if (m.status[0])
                snprintf(svc->notify_status, sizeof svc->notify_status, "%s", m.status);
            if (m.watchdog)
                clock_gettime(CLOCK_MONOTONIC, &svc->last_pet);
            if (m.ready && !svc->notify_ready) {
                svc->notify_ready = 1;
                clock_gettime(CLOCK_MONOTONIC, &svc->last_pet);
                service_log(svc, "notify-ready");
                if (service_verifies_limits(svc)) {
                    /* a limit asked for but not in force fails the service,
                     * never runs it unlimited */
                    char why[256] = "service has no cgroup", ev[288];
                    if (!svc->cgroup_path[0] || !verify_limits_match(svc->cgroup_path, svc, why, sizeof why)) {
                        snprintf(ev, sizeof ev, "limits-mismatch %s", why);
                        service_log(svc, ev);
                        svc->notify_ready = 0;
                        svc_cgroup_kill_tree(svc);
                    }
                }
            }
            break;
        }
        fd_close_detached(rfds, nrfds);
    }
    fdclose_sender_pid = 0;
    fdclose_sender_uid = (uid_t)-1;
}

/* Notify reads paused because every closer is stuck: READY and WATCHDOG
 * cannot arrive, and any local user can cause it. Hold watchdog judgment and
 * start_timeout while it lasts, so the stall cannot kill services or stop
 * the hardware watchdog's pets. Logged with who sent the stuck fds. */
void notify_pause_track(void) {
    int paused = notify_fd >= 0 && fdclose_full();
    if (paused == (notify_paused_since != 0)) return;
    time_t now = mono_sec();
    if (paused) {
        notify_paused_since = now ? now : 1;
        elog(stdout, "notify: reads paused, %d fd closers stuck, %d fds parked; senders:",
               fdclose_n, fdclose_nparked);
        for (int i = 0, shown = 0; i < fdclose_n && shown < 8; i++) {
            int seen = fdclose_from_pid[i] <= 0;
            for (int j = 0; j < i && !seen; j++)
                seen = fdclose_from_pid[j] == fdclose_from_pid[i];
            if (seen) continue;
            printf(" pid %d uid %d", (int)fdclose_from_pid[i], (int)fdclose_from_uid[i]);
            shown++;
        }
        printf("\n");
        return;
    }
    time_t dt = now - notify_paused_since;
    struct timespec pet;
    clock_gettime(CLOCK_MONOTONIC, &pet);
    notify_paused_since = 0;
    for (int i = 0; i < svc_count; i++) {
        service_t *svc = &services[i];
        if (svc->child_pid > 0) {
            svc->last_pet = pet;
            if (svc->wd_abort_at.tv_sec || svc->wd_abort_at.tv_nsec)
                svc->wd_abort_at = pet;
        }
        if (svc->inst.state == STATE_FULL_TRUST && svc->child_pid > 0 && !svc->notify_ready &&
            (svc->notify || svc->ready_bus_name[0])) {
            time_t ran = now - svc->spawn_time_mono.tv_sec;
            svc->spawn_time_mono.tv_sec += dt < ran ? dt : ran;
        }
    }
    elog(stdout, "notify: reads resumed after %lds\n", (long)dt);
}

static int fdstore_keep(const char *name, void *ctx) {
    (void)ctx;
    for (int i = 0; i < svc_count; i++) {
        if (strcmp(services[i].name, name) != 0) continue;
        return services[i].fdstore_max > 0 && services[i].inst.state != STATE_EXCISED &&
               services[i].inst.state != STATE_DORMANT;
    }
    return 0;
}

/* A client that hasn't sent its whole line waits here, polled by the main
 * loop, never read in a blocking loop: a dribbling or silent client used to
 * hold the loop that reaps and pets for up to CTL_RECV_MS per connection. */
#define CTL_PENDING 8
static struct ctl_client {
    int     fd;
    uid_t   uid;
    int     pos;
    int64_t due;
    char    buf[256];
} ctl_pend[CTL_PENDING] = { [0 ... CTL_PENDING - 1] = { .fd = -1 } };

static void ctl_finish(struct ctl_client *c, int complete) {
    int cfd = c->fd;
    struct timeval stv = {CTL_LATE_SEC, 0};
    /* blocking again, so the send timeout governs re-exec's late reply */
    fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL) & ~O_NONBLOCK);
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
    ctl_out_fd = cfd;
    ctl_out_n = 0;
    ctl_out_over = 0;
    if (c->pos > 0 && !complete) {
        /* half of "stop foobar" can name another service */
        ctl_writef(cfd, "err: incomplete command\n");
        ctl_end(cfd);
    } else if (c->pos > 0) {
        c->buf[c->pos] = '\0';
        c->buf[strcspn(c->buf, "\r\n")] = '\0';
        /* Peer identity: root and our own uid may run everything; schema-group
         * members (allowed in by the 0660 socket) get read-only verbs only. */
        if (c->uid != 0 && c->uid != geteuid() && !ctl_is_readonly(c->buf)) {
            ctl_writef(cfd, "err: '%s' requires root (uid %d has read-only access)\n",
                       c->buf, (int)c->uid);
            ctl_end(cfd);
        } else {
            ctl_cmd(cfd, c->buf);
        }
    }
    if (ctl_out_n) send(cfd, ctl_out, ctl_out_n, MSG_DONTWAIT | MSG_NOSIGNAL);
    ctl_out_fd = -1;
    close(cfd);
    c->fd = -1;
}

/* Take what has arrived. 1 if the client is finished (answered or closed). */
static int ctl_client_read(struct ctl_client *c) {
    while (c->pos < (int)sizeof(c->buf) - 1) {
        ssize_t n = recv(c->fd, c->buf + c->pos, 1, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) break;                       /* EOF or error */
        if (c->buf[c->pos++] == '\n') { ctl_finish(c, 1); return 1; }
    }
    ctl_finish(c, 0);
    return 1;
}

static void ctl_poll(void) {
    struct ctl_client *c = NULL;
    struct ucred cred;
    socklen_t clen = sizeof(cred);
    int cfd;
    if (ctl_fd < 0) return;
    uid_t uid = (uid_t)-1;
    int others = 0;
    cfd = accept4(ctl_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0) return;
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0)
        uid = cred.uid;
    for (int i = 0; i < CTL_PENDING; i++) {
        if (ctl_pend[i].fd < 0) { if (!c) c = &ctl_pend[i]; }
        else if (ctl_pend[i].uid != 0 && ctl_pend[i].uid != geteuid()) others++;
    }
    /* the read-only group gets half the slots, so it can't lock root out */
    if (uid != 0 && uid != geteuid() && others >= CTL_PENDING / 2) c = NULL;
    if (!c) {
        send(cfd, "err: busy, try again\n.\n", 23, MSG_DONTWAIT | MSG_NOSIGNAL);
        close(cfd);
        return;
    }
    c->fd = cfd;
    c->pos = 0;
    c->uid = uid;
    c->due = (int64_t)monotonic_ms() + CTL_RECV_MS;
    ctl_client_read(c);                         /* the usual client sent it all already */
}

/* Past the deadline, take only what already arrived: PID 1 being late must
 * not refuse a line the client sent in time. */
static void ctl_expire(void) {
    int64_t now = (int64_t)monotonic_ms();
    for (int i = 0; i < CTL_PENDING; i++)
        if (ctl_pend[i].fd >= 0 && now >= ctl_pend[i].due && !ctl_client_read(&ctl_pend[i]))
            ctl_finish(&ctl_pend[i], 0);
}

/* Clients now outlive one loop iteration: a re-exec or a shutdown would drop
 * them with no reply. */
void ctl_pend_abort(const char *why) {
    char msg[160];
    int n = snprintf(msg, sizeof msg, "err: %s\n.\n", why);
    for (int i = 0; i < CTL_PENDING; i++) {
        if (ctl_pend[i].fd < 0) continue;
        send(ctl_pend[i].fd, msg, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL);
        close(ctl_pend[i].fd);
        ctl_pend[i].fd = -1;
    }
}

static int64_t ctl_due(void) {
    int64_t due = WAKE_NONE;
    for (int i = 0; i < CTL_PENDING; i++)
        if (ctl_pend[i].fd >= 0 && (due == WAKE_NONE || ctl_pend[i].due < due))
            due = ctl_pend[i].due;
    return due;
}

static void signalfd_init(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGCONT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd >= 0) {
        set_nonblock(sig_fd);
    }
}

/* Sleep until the nearest real deadline. The 250 ms tick is kept only for
 * states that are themselves polled. Timers, evictions and dormant backoff
 * used to wake only because some ready_path service forced the tick. */
static int get_poll_timeout(void) {
    struct timespec ts;
    int64_t now_m, now_r, best = WAKE_NONE;
    int64_t host;
    int i, tick = 0, watched = 0, awaiting = 0;

    if (pass_changed)
        return 0;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    now_m = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    host = engine_hooks.wake_due(now_m, &tick);
    /* WAKE_NONE through wake_min would become a finite ~INT64_MAX timeout */
    if (host != WAKE_NONE)
        best = wake_min(best, host, now_m, WAKE_NONE);
    if (reexec_check_pid > 0 && !reexec_check_killed)
        best = wake_min(best, reexec_check_due, now_m, WAKE_NONE);
    clock_gettime(CLOCK_REALTIME, &ts);
    now_r = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    for (i = 0; i < svc_count; i++) {
        service_t *svc = &services[i];
        uint8_t s = svc->inst.state;
        if (s == STATE_FULL_TRUST && svc->ready_path[0] && svc->child_pid > 0 &&
            !(svc->flags & SVC_ONESHOT) &&
            now_m - ((int64_t)svc->spawn_time_mono.tv_sec * 1000 + svc->spawn_time_mono.tv_nsec / 1000000)
                < READY_POLL_SECS * 1000)
            awaiting = 1;
        if (tick)
            continue;
        if (svc->wd_armed_sec > 0 && svc->child_pid > 0 && svc->notify_ready && !svc->ctl_killed &&
            !svc->is_frozen)
            best = wake_min(best, svc_wd_due(&svc->last_pet, &svc->wd_abort_at, svc->wd_armed_sec),
                            now_m, WAKE_NONE);
        /* a runtime stop's escalation deadline; a draining cgroup is polled */
        if (kill_sent_set(svc)) {
            best = wake_min(best, ts_ms_ceil(&svc->kill_sent) + (int64_t)runtime_stop_grace_ms(svc), now_m, WAKE_NONE);
            if (svc->child_pid <= 0) tick = 1;
        }
        if (svc->failsafe_pid > 0 || svc->card_pid > 0 || svc->stop_pid > 0 || svc->reload_pid > 0 ||
            svc->main_wait || (svc->watchdog_timeout_ms > 0 && svc->child_pid > 0))
            tick = 1;
        else if (s == STATE_DORMANT)
            best = wake_min(best, ts_ms_ceil(&svc->dormant_until), now_m, WAKE_NONE);
        else if (s == STATE_NEW_PROCESS && ts_ms_ceil(&svc->restart_at) > now_m)
            best = wake_min(best, ts_ms_ceil(&svc->restart_at), now_m, WAKE_NONE);
        else if (svc_held_on_socket(svc))
            best = wake_min(best, now_m + 1000, now_m, WAKE_NONE);
        else if (s != STATE_FUNDAMENTAL && s != STATE_PERFECT && s != STATE_EXCISED)
            tick = 1;
        else if (s == STATE_FUNDAMENTAL && svc->ready_path[0] && svc->child_pid > 0) {
            if (!svc->ready_path_verified || svc->ready_watched <= 0 || svc->ready_poll_hz > 0)
                tick = 1;
            else
                watched = 1;
        }
        else if (s == STATE_PERFECT && (svc->flags & SVC_TIMER)) {
            if (svc->flags & SVC_TIMER_CALENDAR)
                best = wake_min(best, ts_ms_ceil(&svc->timer_next), now_r, WAKE_CAL_CAP_MS);
            else
                best = wake_min(best, ts_ms_ceil(&svc->timer_next), now_m, WAKE_NONE);
        }
    }
    for (i = 0; i < eviction_count; i++)
        best = wake_min(best, (int64_t)evictions[i].deadline * 1000, now_m, WAKE_NONE);
    for (i = 0; i < svc_count && !watched; i++)
        watched = (services[i].flags & SVC_PATH) != 0;
    if (watched)
        best = wake_min(best, ready_backstop_due, now_m, WAKE_NONE);
    if (ready_event_due)
        best = wake_min(best, ready_event_due, now_m, WAKE_NONE);
    {
        int64_t cd = ctl_due();
        if (cd != WAKE_NONE)
            best = wake_min(best, cd, now_m, WAKE_NONE);
    }
    return wake_timeout(best, tick || awaiting, awaiting ? READY_POLL_MS : TICK_USEC / 1000);
}

int validate_and_resolve(service_t *svc_table, int s_count, group_t *grp_table, int g_count) {
    int g, m, s, d;
    /* resolve group member names → service indices */
    for (g = 0; g < g_count; g++) {
        for (m = 0; m < grp_table[g].member_count; m++) {
            grp_table[g].member_idx[m] = -1;
            for (s = 0; s < s_count; s++) {
                if (strcmp(svc_table[s].name, grp_table[g].member_name[m]) == 0) {
                    grp_table[g].member_idx[m] = s;
                    break;
                }
            }
        }
    }
    /* resolve group deps in service dep_name arrays */
    for (s = 0; s < s_count; s++) {
        for (d = 0; d < MAX_DEPS; d++) {
            if (!svc_table[s].dep_name[d][0]) break;
            svc_table[s].dep_idx[d] = -1;
            svc_table[s].grp_dep_idx[d] = -1;
            /* try to resolve as service dependency first */
            for (int j = 0; j < s_count; j++) {
                if (strcmp(svc_table[j].name, svc_table[s].dep_name[d]) == 0) {
                    svc_table[s].dep_idx[d] = j;
                    if (svc_table[j].flags & SVC_TIMER) {
                        elog(stdout, "WARNING: service '%s' depends on timer service '%s'. Dependents of timer services may be transiently blocked when the timer fires.\n",
                               svc_table[s].name, svc_table[j].name);
                    }
                    break;
                }
            }
            if (svc_table[s].dep_idx[d] >= 0) continue;
            /* resolve as group dependency */
            for (g = 0; g < g_count; g++) {
                if (strcmp(grp_table[g].name, svc_table[s].dep_name[d]) == 0) {
                    svc_table[s].grp_dep_idx[d] = g;
                    break;
                }
            }
        }
    }
    return services_check_cycles(svc_table, s_count);
}

/* Sockets carried from the previous table (re-exec: the old image; reload:
 * the old config): CLOEXEC again, and kept only while they are still what
 * this .svc asks for. A changed listen= is rebound now; a removed one is
 * closed, and a service parked on it starts the plain way. */
void listen_adopt(service_t *svc) {
    int ok = svc->listen_open == svc->listen_count;
    for (int k = 0; ok && k < svc->listen_open; k++)
        ok = service_listen_matches(svc, k);
    if (!ok && svc->listen_open && svc->child_pid > 0) {
        elog(stderr, "%s: listen= changed while running, its sockets are kept until it stops\n", svc->name);
        ok = 1;
    }
    if (!ok) {
        if (svc->listen_open)
            elog(stderr, "%s: carried sockets do not match listen=, rebinding\n", svc->name);
        int parked = svc->listen_open && svc->child_pid == 0 && svc->inst.state == STATE_PERFECT;
        for (int k = 0; k < svc->listen_open && k < MAX_LISTEN; k++)
            close(svc->listen_fd[k]);
        svc->listen_open = 0;
        if (parked && !svc->listen_count) {
            /* a lazy park's hold would park it again, on no socket at all */
            svc->inst.state = STATE_NEW_PROCESS;
            svc->listen_hold = 0;
        } else if (svc->listen_count && service_listen_open(svc) < 0 && parked) {
            svc->inst.state = STATE_NEW_PROCESS;
            svc->listen_hold = 1;
        }
        return;
    }
    for (int k = 0; k < svc->listen_open; k++)
        fcntl(svc->listen_fd[k], F_SETFD, FD_CLOEXEC);
}

/* Returns 0 if the new configuration was adopted, -1 if it was rejected, with
 * `err` filled in. The caller reports that outcome: a reload can be refused for
 * three different reasons and the operator has to be told which, over the same
 * channel they asked on. */
static int handle_reload(int evict_mode, char *err, size_t errsz) {
    int i, j;
    elog(stdout, "reloading configuration (SIGHUP)... \n");

    const char *svc_dir = engine_paths.svc_dir;
    memset(shadow_services, 0, sizeof(service_t) * MAX_SERVICES);
    int shadow_count = services_load(svc_dir, shadow_services, MAX_SERVICES);
    if (svc_refused) {
        elog(stdout, "reload rejected: %d file(s)/dir(s) refused on owner/mode\n", svc_refused);
        if (err) snprintf(err, errsz, "%d .svc file(s) or service dir(s) refused: not root-owned or writable "
                          "by others — see the REFUSED/WARN lines (chown root:root; chmod go-w)", svc_refused);
        for (i = 0; i < shadow_count; i++) {
            for (j = 1; j < MAX_ARGV; j++)
                free(shadow_services[i].argv[j]);
            service_free_landlock(&shadow_services[i]);
        }
        return -1;
    }
    if (shadow_count == 0 && engine_paths.svc_fallback) {
        shadow_count = services_load(engine_paths.svc_fallback, shadow_services, MAX_SERVICES);
    }
    /* a missing dir also loads zero: that is a half-done import swap, not
     * "nothing enabled" */
    if (shadow_count < 0 || (shadow_count == 0 &&
        (!engine_reload_allow_empty || access(svc_dir, F_OK) != 0))) {
        elog(stdout, "reload failed: no services loaded from configuration\n");
        if (err) snprintf(err, errsz, "no services loaded from configuration");
        return -1;
    }

    memset(shadow_groups, 0, sizeof(group_t) * MAX_GROUPS);
    int shadow_grp_count = groups_load(svc_dir, shadow_groups, MAX_GROUPS);
    if (shadow_grp_count == 0 && engine_paths.svc_fallback) {
        shadow_grp_count = groups_load(engine_paths.svc_fallback, shadow_groups, MAX_GROUPS);
    }

    /* hash integrity check — reject any .svc file modified since boot */
    for (i = 0; engine_reload_integrity && i < shadow_count; i++) {
        for (j = 0; j < svc_count; j++) {
            if (strcmp(shadow_services[i].name, services[j].name) != 0) continue;
            if (shadow_services[i].content_hash != services[j].content_hash &&
                shadow_services[i].content_hash != 0) {
                elog(stdout, "INTEGRITY: '%s' hash mismatch — file modified since boot; reload rejected\n",
                       shadow_services[i].name);
                if (err) snprintf(err, errsz,
                    "'%s' modified since boot — integrity check rejected the reload; "
                    "a changed .svc takes effect at next boot",
                    shadow_services[i].name);
                for (int k = 0; k < shadow_count; k++)
                    for (int m = 1; m < MAX_ARGV; m++)
                        if (shadow_services[k].argv[m]) { free(shadow_services[k].argv[m]); shadow_services[k].argv[m] = NULL; }
                for (int k = 0; k < shadow_count; k++) service_free_landlock(&shadow_services[k]);
                return -1;
            }
            break;
        }
    }

    if (validate_and_resolve(shadow_services, shadow_count, shadow_groups, shadow_grp_count) > 0) {
        elog(stdout, "reload rejected: dependency cycles detected in new configuration\n");
        if (err) snprintf(err, errsz, "dependency cycle in new configuration");
        /* free duplicated arguments in shadow array to prevent leaks */
        for (i = 0; i < shadow_count; i++) {
            for (j = 1; j < MAX_ARGV; j++) {
                if (shadow_services[i].argv[j]) {
                    free(shadow_services[i].argv[j]);
                }
            }
            service_free_landlock(&shadow_services[i]);
        }
        return -1;
    }

    /* Merge running state from live to shadow */
    struct timespec reload_now;
    clock_gettime(CLOCK_MONOTONIC, &reload_now);
    for (i = 0; i < shadow_count; i++) {
        for (j = 0; j < svc_count; j++) {
            if (strcmp(shadow_services[i].name, services[j].name) == 0) {
                /* cgroup_path and is_frozen are runtime-only and ride along:
                 * without them the PSI freeze/reclaim executive no-ops after a
                 * reload, and a service frozen at reload time can never thaw. */
                svc_runtime_copy(&shadow_services[i], &services[j]);
                svc_clamp_hold(&shadow_services[i], &reload_now);
                /* the copy brought the old listen_fd[]; a changed or dropped
                 * listen= kept the old socket until reboot */
                if (shadow_services[i].listen_open)
                    listen_adopt(&shadow_services[i]);

                /* A run-once timer (on_boot_sec with no interval) marks itself
                 * terminal by clearing SVC_TIMER when it completes. The shadow
                 * is parsed fresh from the .svc, so it has the flag back, and
                 * it just inherited the live timer_next -- a deadline already
                 * in the past. The arm block at the end of this function only
                 * touches timers whose timer_next is zero, so the shadow would
                 * keep that expired deadline and fire on the very next tick.
                 * Carry the terminal fact across instead. Without this, every
                 * `schema-ctl reload` re-ran every completed boot timer:
                 * boot-timing reached restarts=6 in one afternoon and wrote
                 * six phantom rows into boot-history.csv, all claiming the
                 * same boot. */
                svc_carry_timer_done(&shadow_services[i], !(services[j].flags & SVC_TIMER));

                /* clear live service state so we don't accidentally treat it as running/managed */
                services[j].child_pid = 0;
                services[j].failsafe_pid = 0;
                services[j].listen_open = 0;
                break;
            }
        }
    }

    /* Identify and clean up orphaned running services */
    for (j = 0; j < svc_count; j++) {
        if (services[j].child_pid > 0 || services[j].failsafe_pid > 0) {
            elog(stdout, "service '%s' is orphaned in new configuration\n", services[j].name);
            if (evict_mode) {
                elog(stdout, "evicting orphaned service '%s' (pid=%d)\n", services[j].name, (int)services[j].child_pid);
                if (services[j].child_pid > 0) {
                    kill(services[j].child_pid, SIGTERM);
                    /* the service struct goes with this reload, and with it
                     * the cpuset needed to give an isolated partition's
                     * cores back; eviction_drop() only rmdirs, so do it now */
                    service_cgroup_release_partition(&services[j]);
                    eviction_add(services[j].child_pid, services[j].cgroup_path);
                }
                /* its stop commands can't be sequenced without the struct:
                 * one already running is tracked, else both lists run now,
                 * beside the SIGTERM */
                if (services[j].reload_pid > 0) eviction_add(services[j].reload_pid, "");
                if (services[j].stop_pid > 0) {
                    eviction_add(services[j].stop_pid, "");
                } else if (services[j].child_pid > 0 &&
                           (services[j].exec_stop_count || services[j].exec_stop_post_count)) {
                    pid_t sp = service_stop_spawn(&services[j], 3);
                    /* the service's own stop budget: podman rm -f waits 10 s */
                    int g = services[j].stop_timeout_sec > EVICT_GRACE_SECS ? services[j].stop_timeout_sec
                                                                           : EVICT_GRACE_SECS;
                    if (sp > 0) eviction_add_as(sp, "", 0, 0, g);
                }
                if (services[j].failsafe_pid > 0) {
                    kill(services[j].failsafe_pid, SIGTERM);
                }
            } else {
                elog(stdout, "service '%s' (pid=%d) running unmanaged to natural death\n", services[j].name, (int)services[j].child_pid);
            }
        }
        service_listen_close(&services[j]);
        /* Free duplicated arguments from the old configuration array */
        for (int k = 1; k < MAX_ARGV; k++) {
            if (services[j].argv[k]) {
                free(services[j].argv[k]);
                services[j].argv[k] = NULL;
            }
        }
        service_free_landlock(&services[j]);
    }

    /* Swap the buffers */
    service_t *tmp_svc = services;
    services = shadow_services;
    shadow_services = tmp_svc;
    svc_count = shadow_count;

    group_t *tmp_grp = groups;
    groups = shadow_groups;
    shadow_groups = tmp_grp;
    grp_count = shadow_grp_count;

    elog(stdout, "configuration reload completed successfully (generation updated)\n");
    service_cgroup_root_low(services, svc_count);
    eviction_readded();

    dynuser_apply_all();
    for (i = 0; i < svc_count; i++) {
        service_t *s = &services[i];
        if (s->flags & SVC_PATH) {
            path_arm(s);
        } else if (s->path_armed) {
            /* watch_path= removed: an armed one is now a plain service, so start it */
            s->path_armed = s->path_due = s->path_rerun = 0;
            if (s->child_pid <= 0 && s->inst.state == STATE_PERFECT) s->inst.state = STATE_NEW_PROCESS;
        }
    }
    path_gc();

    /* arm only timers new to the config: a carried deadline is kept, never re-drawn */
    for (i = 0; i < svc_count; i++)
        if ((services[i].flags & SVC_TIMER) && services[i].timer_next.tv_sec == 0)
            timer_arm(&services[i]);
    return 0;
}

int engine_reload(int evict, char *err, size_t errsz) {
    return handle_reload(evict, err, errsz);
}

const group_t *engine_groups(int *n) {
    *n = grp_count;
    return groups;
}

int engine_load(const char *svc_dir) {
    svc_count = services_load(svc_dir, services, MAX_SERVICES);
    if (svc_count == 0 && engine_paths.svc_fallback)
        svc_count = services_load(engine_paths.svc_fallback, services, MAX_SERVICES);
    grp_count = groups_load(svc_dir, groups, MAX_GROUPS);
    if (grp_count == 0 && engine_paths.svc_fallback)
        grp_count = groups_load(engine_paths.svc_fallback, groups, MAX_GROUPS);
    return validate_and_resolve(services, svc_count, groups, grp_count);
}
#define SHUT_TOTAL_MS  12000
#define SHUT_SLOW_MS     250
#define SHUT_STUCK_MS   1000   /* how long a SIGKILLed service gets to be gone */

static int shut_depends_on(const service_t *s, int target) {
    int k, m;
    for (k = 0; k < MAX_DEPS && s->dep_name[k][0]; k++) {
        int gi = s->grp_dep_idx[k];
        if (s->dep_idx[k] == target) return 1;
        if (gi >= 0)
            for (m = 0; m < groups[gi].member_count; m++)
                if (groups[gi].member_idx[m] == target) return 1;
    }
    return 0;
}

/* Signal every process in a service's cgroup, main PID included; sig 0 just
 * counts the live ones. A service is down only when this reaches 0. */
static int svc_signal_all(service_t *svc, int sig) {
    char path[320];
    FILE *f;
    pid_t p;
    int live = 0;

    if (svc->child_pid > 0 && kill(svc->child_pid, sig) == 0) live++;
    if (!svc->cgroup_path[0]) return live;
    snprintf(path, sizeof path, "%s/cgroup.procs", svc->cgroup_path);
    if (!(f = fopen(path, "r"))) return live;
    while (fscanf(f, "%d", &p) == 1)
        if (p > 1 && p != svc->child_pid && kill(p, sig) == 0) live++;
    fclose(f);
    return live;
}

static void shut_reap(void) {
    pid_t pid;
    int i;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        if (fdclose_reaped(pid)) continue;
        for (i = 0; i < svc_count; i++) {
            if (services[i].stop_pid == pid) { services[i].stop_pid = 0; services[i].stop_phase = 0; break; }
            if (services[i].reload_pid == pid) { services[i].reload_pid = 0; break; }
            if (services[i].child_pid == pid) { services[i].child_pid = 0; break; }
        }
    }
}

/* Stop in reverse dependency order: a service gets SIGTERM once nothing still
 * running depends on it, then has SHUT_GRACE_MS (or its stop_timeout_sec) to
 * exit before its cgroup is killed. A straggler holds back only what it depends on. Past SHUT_TOTAL_MS
 * everything left gets SIGTERM at once. stop_first services go before all of it. */
static void shutdown_ordered(void) {
    uint64_t start = monotonic_ms(), term_at[MAX_SERVICES] = {0}, stop_at[MAX_SERVICES] = {0},
             post_at[MAX_SERVICES] = {0};
    int alive[MAX_SERVICES], killed[MAX_SERVICES] = {0}, gone[MAX_SERVICES] = {0}, was_up[MAX_SERVICES];
    int told[MAX_SERVICES] = {0};
    int i, j, round = 0, held = 0, first = 1;
    char msg[320];

    for (;;) {
        uint64_t now = monotonic_ms();
        int left = 0, sent = 0;
        size_t off = 0;

        shut_reap();
        for (i = 0; i < svc_count; i++) {
            if (services[i].kill_mode == KILL_MODE_PROCESS) {
                /* only the main process is waited for; the rest go with the final sweep */
                alive[i] = !killed[i] && services[i].child_pid > 0 && kill(services[i].child_pid, 0) == 0;
            } else {
                /* mixed: once the main process is gone, the rest get final_kill_signal */
                if (services[i].kill_mode == KILL_MODE_MIXED && term_at[i] && !killed[i] &&
                    services[i].child_pid <= 0 && svc_signal_all(&services[i], 0) > 0)
                    final_signal(&services[i]);
                alive[i] = !killed[i] && svc_signal_all(&services[i], 0) > 0;
            }
            if (first) was_up[i] = alive[i];
            /* exec_stop_post once a service that was up is gone, however it went:
             * SIGTERM, a kill, its own exit, or exec_stop taking it down (podman
             * rm, kill $MAINPID). A stop runner still going is waited for within
             * its grace, then killed: never an unbounded hold on shutdown. */
            if (!alive[i] && was_up[i] && !post_at[i]) {
                if (services[i].stop_pid > 0) {
                    const struct timespec *t = &services[i].stop_start;
                    uint64_t ran = now - ((uint64_t)t->tv_sec * 1000 + (uint64_t)t->tv_nsec / 1000000);
                    uint64_t grace = stop_grace_ms(&services[i]);
                    if (ran >= grace) kill_group(services[i].stop_pid);
                    /* past the kill, a second for the reap so post can still start;
                     * one wedged in D state is given up on, not waited for */
                    if (!killed[i] && ran < grace + 1000) alive[i] = 1;
                } else if (stop_cmds_begin(&services[i], 2)) {
                    post_at[i] = now;
                }
            }
            if (post_at[i] && services[i].stop_pid > 0) {
                if (now - post_at[i] < stop_grace_ms(&services[i])) alive[i] = 1;
                else kill_group(services[i].stop_pid);
            }
            left += alive[i];
            /* was_up is the snapshot taken on the first pass: each gets one verdict */
            if (!alive[i] && was_up[i] && !term_at[i] && !killed[i] && !told[i]) {
                told[i] = 1;
                engine_hooks.shut_outcome(i, stop_at[i] ? SHUT_STOPPED : SHUT_EXITED);
            }
            if (!alive[i] && term_at[i] && !killed[i] && !gone[i]) {
                gone[i] = 1;
                if (!told[i]) { told[i] = 1; engine_hooks.shut_outcome(i, SHUT_STOPPED); }
                if (now - term_at[i] >= SHUT_SLOW_MS) {
                    snprintf(msg, sizeof msg, "%s down at +%llu ms (%llu ms after SIGTERM)",
                             services[i].name, (unsigned long long)(now - start),
                             (unsigned long long)(now - term_at[i]));
                    engine_hooks.shut_log(msg);
                }
            }
        }
        if (!left) break;
        /* stop_first services (the desktop session) go before anything else:
         * their clients would otherwise block on daemons stopping under them */
        int hold = 0;
        for (i = 0; i < svc_count; i++)
            if (alive[i] && (services[i].flags & SVC_STOP_FIRST)) hold = 1;
        if (held && !hold) {
            snprintf(msg, sizeof msg, "stop_first services down in %llu ms",
                     (unsigned long long)(now - start));
            engine_hooks.shut_log(msg);
            start = now;
        }
        held = hold;
        for (i = 0; i < svc_count; i++) {
            int blocked = 0;
            if (!alive[i]) continue;
            if (term_at[i]) {
                uint64_t grace = stop_grace_ms(&services[i]);
                if (now - term_at[i] < grace) continue;
                snprintf(msg, sizeof msg, "%s ignored SIGTERM for %llu ms, killing",
                         services[i].name, (unsigned long long)grace);
                engine_hooks.shut_log(msg);
                final_signal(&services[i]);
                killed[i] = 1;   /* its verdict waits for the kill to take, below */
                continue;
            }
            if (hold && !(services[i].flags & SVC_STOP_FIRST)) continue;
            if (now - start < SHUT_TOTAL_MS)
                for (j = 0; j < svc_count && !blocked; j++)
                    blocked = j != i && alive[j] && shut_depends_on(&services[j], i);
            if (blocked) continue;
            /* exec_stop first (or one already running from schema-ctl stop);
             * SIGTERM once it is done or out of time */
            if (!stop_at[i] && (services[i].stop_pid > 0 ||
                                (services[i].exec_stop_count && stop_cmds_begin(&services[i], 1)))) {
                stop_at[i] = now;
                continue;
            }
            if (stop_at[i] && services[i].stop_pid > 0) {
                if (now - stop_at[i] < stop_grace_ms(&services[i])) continue;
                kill_group(services[i].stop_pid);
            }
            if (services[i].kill_mode == KILL_MODE_PROCESS || services[i].kill_mode == KILL_MODE_MIXED) {
                /* never kill(0, ...): that is PID 1's own process group */
                if (services[i].child_pid > 0) kill(services[i].child_pid, kill_sig(&services[i]));
                else final_signal(&services[i]);
            } else
                svc_signal_all(&services[i], kill_sig(&services[i]));
            term_at[i] = now;
            if (!sent++)
                off = snprintf(msg, sizeof msg, "stop %d (+%llu ms):", ++round,
                               (unsigned long long)(now - start));
            if (off < sizeof msg)
                off += snprintf(msg + off, sizeof msg - off, " %s", services[i].name);
        }
        if (sent) engine_hooks.shut_log(msg);
        first = 0;
        usleep(20000);
    }
    /* A kill is instant but the exit and the reap are not: give the killed up to
     * SHUT_STUCK_MS to go, then KILLED or STUCK (D state) — one verdict each.
     * process mode judges only the main pid; its leftovers are the final sweep's. */
    for (uint64_t until = monotonic_ms() + SHUT_STUCK_MS;;) {
        int pending = 0;
        shut_reap();
        for (i = 0; i < svc_count; i++) {
            if (!killed[i] || told[i]) continue;
            if (services[i].kill_mode == KILL_MODE_PROCESS
                    ? services[i].child_pid > 0 && kill(services[i].child_pid, 0) == 0
                    : svc_signal_all(&services[i], 0) > 0)
                pending = 1;
            else { told[i] = 1; engine_hooks.shut_outcome(i, SHUT_KILLED); }
        }
        if (!pending || monotonic_ms() >= until) break;
        usleep(20000);
    }
    for (i = 0; i < svc_count; i++)
        if (killed[i] && !told[i]) { told[i] = 1; engine_hooks.shut_outcome(i, SHUT_STUCK); }
    snprintf(msg, sizeof msg, "services down in %llu ms",
             (unsigned long long)(monotonic_ms() - start));
    engine_hooks.shut_log(msg);
}


void engine_open(int adopting) {
    service_notify_sock = engine_paths.notify_sock;
    shm_init();
    mkdir(engine_paths.run_dir, 0755);
    if (!adopting) {
        ctl_init();
        notify_fd = notify_open(engine_paths.notify_sock);
        fdclose_sock = notify_fd;
    }
    engine_hooks.after_sockets();
    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    path_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (adopting) {
        /* Children that exited during the swap are zombies only the restored
         * child_pid can attribute; their SIGCHLD stays pending for signalfd. */
        reap();
        ready_event_due = (int64_t)monotonic_ms();
        /* the flag isn't carried; a service already promoted on its name proves a reporter */
        for (int i = 0; i < svc_count; i++)
            if (services[i].ready_bus_name[0] && services[i].notify_ready) bus_reporter_seen = 1;
    }
    signalfd_init();
}

/* arm timer services: born "already ran", first fire = boot + on_boot_sec.
 * After a re-exec only timers new to the config (timer_next still 0). */
void engine_arm_timers(int adopting) {
    dynuser_apply_all();
    for (int ti = 0; ti < svc_count; ti++)
        if (services[ti].flags & SVC_PATH) path_arm(&services[ti]);
    for (int ti = 0; ti < svc_count; ti++)
        if ((services[ti].flags & SVC_TIMER) && (!adopting || services[ti].timer_next.tv_sec == 0))
            timer_arm(&services[ti]);
}

void engine_run(void) {
    int i;

    while (engine_running) {
        struct pollfd fds[6 + CTL_PENDING + MAX_SERVICES * MAX_LISTEN];
        int fd_svc[6 + CTL_PENDING + MAX_SERVICES * MAX_LISTEN];
        int nfds = 0, nfixed;
        int psi_fired = 0;
        uint8_t grp_states[MAX_GROUPS];
        uint8_t svc_states[MAX_SERVICES];
        int ret = 0;

        if (sig_fd >= 0) {
            fds[nfds].fd = sig_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        if (ctl_fd >= 0) {
            fds[nfds].fd = ctl_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        /* paused, notify_poll reads nothing; polling would spin */
        notify_pause_track();
        if (notify_fd >= 0 && !notify_paused_since) {
            fds[nfds].fd = notify_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        if (psi_fd >= 0) {
            fds[nfds].fd = psi_fd;
            fds[nfds].events = POLLPRI;
            fds[nfds].revents = 0;
            nfds++;
        }
        if (ino_fd >= 0) {
            fds[nfds].fd = ino_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        if (path_fd >= 0) {
            fds[nfds].fd = path_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        int npend = nfds;
        for (int ci = 0; ci < CTL_PENDING; ci++) {
            if (ctl_pend[ci].fd < 0) continue;
            fds[nfds].fd = ctl_pend[ci].fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            fd_svc[nfds++] = ci;
        }
        nfixed = nfds;
        for (int si = 0; si < svc_count; si++) {
            if (!service_listen_waiting(&services[si])) continue;
            for (int k = 0; k < services[si].listen_open; k++) {
                fds[nfds].fd = services[si].listen_fd[k];
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                fd_svc[nfds++] = si;
            }
        }

        if (nfds > 0) {
            ret = poll(fds, nfds, get_poll_timeout());
            if (ret > 0) {
                int e;
                for (e = nfixed; e < nfds; e++) {
                    service_t *ls = &services[fd_svc[e]];
                    if (fds[e].revents && service_listen_waiting(ls)) {
                        ls->inst.state = STATE_NEW_PROCESS;
                        ls->listen_hold = 0;
                        service_log(ls, "socket-wake");
                    }
                }
                for (e = 0; e < npend; e++) {
                    if (fds[e].fd == psi_fd && psi_fd >= 0) {
                        if (fds[e].revents & (POLLERR | POLLNVAL)) {
                            close(psi_fd);
                            psi_fd = -1;
                            elog(stdout, "memory pressure: psi trigger lost, avg10 polling\n");
                        } else if (fds[e].revents & POLLPRI) {
                            psi_fired = 1;
                        }
                        continue;
                    }
                    if (!(fds[e].revents & POLLIN)) continue;
                    if (fds[e].fd == sig_fd) {
                        struct signalfd_siginfo ssi;
                        while (read(sig_fd, &ssi, sizeof(ssi)) == (ssize_t)sizeof(ssi)) {
                            if (ssi.ssi_signo == SIGCHLD) {
                                reap();
                            } else if (ssi.ssi_signo == SIGHUP) {
                                /* No socket to answer on — the console message
                                 * inside handle_reload is the only report. */
                                (void)handle_reload(0, NULL, 0);
                            } else if (ssi.ssi_signo == SIGCONT) {
                                engine_hooks.on_sigcont();
                            }
                        }
                    } else if (fds[e].fd == ctl_fd) {
                        ctl_poll();
                    } else if (fds[e].fd == notify_fd) {
                        notify_poll(NOTIFY_BATCH_MAX);
                    } else if (fds[e].fd == ino_fd) {
                        ready_inotify_drain();
                    } else if (fds[e].fd == path_fd) {
                        path_drain();
                    }
                }
                /* after sig_fd: a command must see the deaths reaped this wake */
                for (e = npend; e < nfixed; e++) {
                    struct ctl_client *cc = &ctl_pend[fd_svc[e]];
                    if (fds[e].revents && cc->fd == fds[e].fd)
                        ctl_client_read(cc);
                }
            } else if (ret < 0 && errno != EINTR) {
                usleep(TICK_USEC);
            }
        } else {
            usleep(TICK_USEC);
        }

        ctl_expire();
        engine_hooks.loop_pressure(psi_fired);

        {
            int64_t now = (int64_t)monotonic_ms();
            if ((ready_event_due && now >= ready_event_due) || now >= ready_backstop_due) {
                if (now >= ready_backstop_due) path_sweep();
                ready_event_due = 0;
                ready_backstop_due = now + READY_BACKSTOP_MS;
                ready_recheck_all();
            }
        }

        for (i = 0; i < grp_count; i++)  grp_states[i] = groups[i].state;
        for (i = 0; i < svc_count; i++) {
            uint8_t s = services[i].inst.state;
            if ((services[i].flags & (SVC_TIMER | SVC_PATH)) && (s == STATE_NEW_PROCESS || s == STATE_FULL_TRUST)) {
                svc_states[i] = STATE_PERFECT;
            } else {
                svc_states[i] = s;
            }
        }
        uint8_t before[MAX_SERVICES];
        for (i = 0; i < svc_count; i++)
            before[i] = services[i].inst.state;
        timer_catchup_tick();
        bind_tick();
        for (i = 0; i < svc_count; i++)
            tick_service(&services[i], grp_states, grp_count);
        {
            static int streak;
            int moved = 0;
            for (i = 0; i < svc_count && !moved; i++)
                moved = services[i].inst.state != before[i];
            streak = moved ? (pass_changed ? streak + 1 : 1) : 0;
            pass_changed = moved && streak <= PASS_RERUN_MAX;
        }
        if (fdstore_n > 0) fdstore_sweep(fdstore_keep, NULL);
        monitor_failsafes();
        groups_update(groups, grp_count, svc_states, svc_count);
        shm_update();
        eviction_tick();
        runtime_dir_tick();
        service_watchdog_tick();
        bind_snapshot_up();
        engine_hooks.loop_tail();
        engine_reexec_tick();
    }
}

void engine_stop_services(void) {
    int i;

    for (i = 0; i < svc_count; i++)
        service_listen_close(&services[i]);
    shutdown_ordered();
}

void engine_kill_services(void) {
    int i;

    for (i = 0; i < svc_count; i++) {
        svc_cgroup_kill_tree(&services[i]);
    }
}

void engine_release(void) {
    if (notify_fd >= 0) {
        close(notify_fd);
        unlink(engine_paths.notify_sock);
    }
    ctl_pend_abort("shutting down");
    if (ctl_fd >= 0) {
        close(ctl_fd);
        unlink(engine_paths.ctl_sock);
        if (engine_paths.ctl_fallback) unlink(engine_paths.ctl_fallback);
    }

    if (shm_ptr) {
        munmap(shm_ptr, sizeof(schema_shm_t));
        shm_unlink(engine_paths.shm_name);
    }
}

void engine_free(void) {
    int i;

    /* free duplicated arguments to prevent memory leaks */
    for (i = 0; i < svc_count; i++) {
        int j;
        for (j = 1; j < MAX_ARGV; j++) {
            if (services[i].argv[j]) {
                free(services[i].argv[j]);
                services[i].argv[j] = NULL;
            }
        }
        service_free_landlock(&services[i]);
    }
}

/* ── re-exec (shared by PID 1 and schema-user) ─────────────────────── */

#define REEXEC_CHECK_MS 5000
static int     reexec_check_out = -1;     /* read end of the dry run's stdout */
static int     reexec_client = -1;        /* the waiting schema-ctl connection */
static char    reexec_path[256];
static int     reexec_target = -1;        /* the binary the dry run validated */
char           engine_reexec_argv0[256];
int            engine_reexec_keep_argv0 = 0;
char         **engine_reexec_env = NULL;
int            engine_reexec_lock_fd = -1;
static void reexec_check_done(int status);

static char **rx_env(void) { return engine_reexec_env ? engine_reexec_env : environ; }

/* The signals that end PID 1. Blocked across execve: the kernel drops
 * SIG_DFL signals sent to PID 1, and execve resets our handlers to SIG_DFL. */
static void held_signals(sigset_t *set) {
    sigemptyset(set);
    sigaddset(set, SIGTERM);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGUSR1);
    sigaddset(set, SIGUSR2);
}

/* ── re-exec: the new image ─────────────────────────────────────────── */

static reexec_svc_t reexec_svcs[MAX_SERVICES];
static int reexec_reply_fd = -1;
static char reexec_reply[160];

static int reexec_read_blob(int fd, reexec_global_t *g, int *nrs,
                            eviction_t *ev, int *nev, fdstore_ent *fs, int maxfs, int *nfs,
                            char *err, size_t errsz) {
    int d;
    FILE *f;
    if (lseek(fd, 0, SEEK_SET) < 0 || (d = dup(fd)) < 0) {
        snprintf(err, errsz, "state fd %d: %s", fd, strerror(errno));
        return -1;
    }
    if (!(f = fdopen(d, "r"))) {
        snprintf(err, errsz, "state fd %d: %s", fd, strerror(errno));
        close(d);
        return -1;
    }
    int rc = state_parse(f, g, reexec_svcs, MAX_SERVICES, nrs, ev, nev, fs, maxfs, nfs, err, errsz);
    fclose(f);
    return rc;
}

/* Everything the new image must prove before it touches anything: the config
 * parses without a cycle, the blob parses, and the two agree. */
static int reexec_validate(int blob_fd, reexec_global_t *g, int *nrs,
                           eviction_t *ev, int *nev, fdstore_ent *fs, int maxfs, int *nfs,
                           char *err, size_t errsz) {
    if (engine_load(engine_paths.svc_dir) > 0) {
        snprintf(err, errsz, "dependency cycle in %s", engine_paths.svc_dir);
        return -1;
    }
    if (svc_count < 0 || (svc_count == 0 && !engine_reload_allow_empty)) {
        snprintf(err, errsz, "no services loaded from %s", engine_paths.svc_dir);
        return -1;
    }
    if (reexec_read_blob(blob_fd, g, nrs, ev, nev, fs, maxfs, nfs, err, errsz) < 0)
        return -1;
    return reexec_compare(reexec_svcs, *nrs, services, svc_count, err, errsz);
}

/* --reexec-check: run by the old image as a plain child, never as PID 1. */
int engine_reexec_check_main(int blob_fd) {
    reexec_global_t g;
    eviction_t ev[MAX_EVICTIONS];
    int nrs, nev, nfs, rc;
    char err[320];
    /* stdout is the verdict pipe: the loader's chatter goes to stderr, so
     * the verdict is the only thing on it however much the loader says */
    int verdict = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    /* the default changes what a .svc loads as, so the check must see it too */
    engine_hooks.reexec_check_prep();
    rc = reexec_validate(blob_fd, &g, &nrs, ev, &nev, NULL, 0, &nfs, err, sizeof err);
    fflush(stdout);
    if (rc < 0) {
        dprintf(verdict, "err: %s\n", err);
        return 1;
    }
    dprintf(verdict, "ok: %s\n", SCHEMA_INIT_VERSION);
    return 0;
}

/* The new image could not take the state. Hand it back to the image that
 * wrote it; that image runs this same path and adopts it. If that is gone
 * too, a clean reboot beats a PID 1 exit (panic, and a hang without a
 * hardware watchdog). */
static void reexec_fallback(int blob_fd, int oldexe_fd, int client_fd, const char *why) {
    char b[16], o[16];
    fprintf(stderr, "[%s] re-exec refused by the new image: %s; returning to the previous image\n", engine_paths.tag, why);
    if (client_fd >= 0)
        ctl_writef(client_fd, "err: new image refused the state: %s; back on the previous image\n", why);
    if (oldexe_fd >= 0) {
        /* "-1": the previous image has nowhere further back to go, so it
         * adopts without the strict checks instead of refusing in a loop */
        char *av[] = { engine_reexec_keep_argv0 ? engine_reexec_argv0 : "/sbin/schema-init", "--reexec", b, o, NULL };
        snprintf(b, sizeof b, "%d", blob_fd);
        snprintf(o, sizeof o, "%d", -1);
        fcntl(oldexe_fd, F_SETFD, FD_CLOEXEC);
        fflush(NULL);
        fexecve(oldexe_fd, av, rx_env());
    }
    fprintf(stderr, "[%s] re-exec: previous image unavailable (%s)\n", engine_paths.tag, strerror(errno));
    engine_hooks.reexec_last_resort();
    _exit(1);
}

/* Close every descriptor the old image leaked across execve, keeping the
 * standard three and the ones the blob names. */
static void reexec_close_strays(const int *keep, int nkeep) {
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int strays[256], n = 0;
    if (!d) return;
    int dfd = dirfd(d);
    while ((e = readdir(d))) {
        char *end;
        long fd = strtol(e->d_name, &end, 10);
        if (*end || end == e->d_name || fd <= 2 || fd == dfd) continue;
        int k;
        for (k = 0; k < nkeep; k++)
            if (keep[k] == fd) break;
        if (k < nkeep) continue;
        strays[n++] = (int)fd;
        if (n == (int)(sizeof strays / sizeof strays[0])) {
            fd_close_detached(strays, n);
            n = 0;
        }
    }
    closedir(d);
    fd_close_detached(strays, n);
}

void engine_reexec_adopt(int blob_fd, int oldexe_fd) {
    reexec_global_t g;
    eviction_t ev[MAX_EVICTIONS];
    int nrs = 0, nev = 0, nfs = 0, adopted = 0, fresh = 0, rolled_back = oldexe_fd < 0;
    char err[320];
    fdstore_ent *fs = calloc(FDSTORE_MAX_TOTAL, sizeof *fs);
    if (!fs)
        fprintf(stderr, "[%s] re-exec: out of memory, fdstore dropped across re-exec\n", engine_paths.tag);

    memset(&g, 0, sizeof g);
    g.fd_ctl = g.fd_notify = g.fd_watchdog = g.fd_client = g.fd_oldexe = g.fd_lock = -1;
    if (oldexe_fd >= 0) {
        if (reexec_validate(blob_fd, &g, &nrs, ev, &nev, fs, fs ? FDSTORE_MAX_TOTAL : 0, &nfs, err, sizeof err) < 0)
            reexec_fallback(blob_fd, oldexe_fd, g.fd_client, err);
    } else {
        /* We ARE the fallback: nowhere further back to go, so refusing would
         * loop. Adopt whatever the config and blob allow; only a blob we
         * cannot read at all stops us, since then there is nothing to adopt. */
        int cycles = engine_load(engine_paths.svc_dir);
        if (reexec_read_blob(blob_fd, &g, &nrs, ev, &nev, fs, fs ? FDSTORE_MAX_TOTAL : 0, &nfs, err, sizeof err) < 0)
            reexec_fallback(blob_fd, -1, g.fd_client, err);
        if (cycles > 0)
            fprintf(stderr, "[%s] re-exec fallback: dependency cycle in %s, adopting anyway\n", engine_paths.tag, engine_paths.svc_dir);
        if (reexec_compare(reexec_svcs, nrs, services, svc_count, err, sizeof err) < 0)
            fprintf(stderr, "[%s] re-exec fallback: adopting despite: %s\n", engine_paths.tag, err);
    }

    {
        static int keep[7 + MAX_SERVICES * MAX_LISTEN + FDSTORE_MAX_TOTAL];
        int nkeep = 0, nres;
        keep[nkeep++] = g.fd_ctl; keep[nkeep++] = g.fd_notify; keep[nkeep++] = g.fd_watchdog;
        keep[nkeep++] = g.fd_lock;
        keep[nkeep++] = g.fd_client; keep[nkeep++] = blob_fd; keep[nkeep++] = oldexe_fd;
        for (int j = 0; j < nrs; j++)
            for (int k = 0; k < reexec_svcs[j].rt.listen_open && k < MAX_LISTEN; k++)
                keep[nkeep++] = reexec_svcs[j].rt.listen_fd[k];
        nres = nkeep;
        for (int j = 0; j < nfs; j++)
            keep[nkeep++] = fs[j].fd;
        reexec_close_strays(keep, nkeep);
        /* a blob entry naming an fd PID 1 already holds would make the store
         * own, and later close, PID 1's own socket: skip it */
        for (int j = 0; j < nfs; j++) {
            int fd = fs[j].fd, dup = 0;
            for (int k = 0; k < nres && !dup; k++)
                dup = keep[k] == fd;
            for (int k = 0; k < fdstore_n && !dup; k++)
                dup = fdstore_v[k].fd == fd;
            if (dup) {
                fprintf(stderr, "[%s] re-exec: fdstore %s/%s: fd %d is not ours, skipped\n", engine_paths.tag,
                        fs[j].svc, fs[j].name, fd);
                continue;
            }
            if (fcntl(fd, F_GETFD) < 0) {
                fprintf(stderr, "[%s] re-exec: fdstore %s/%s: fd %d is not open, skipped\n", engine_paths.tag,
                        fs[j].svc, fs[j].name, fd);
                continue;
            }
            if (fdstore_add(fs[j].svc, fd, fs[j].name, FDSTORE_MAX_TOTAL) == 0)
                fcntl(fd, F_SETFD, FD_CLOEXEC);
            else
                fd_close_detached(&fd, 1);
        }
    }
    free(fs);

    ctl_fd = g.fd_ctl;
    notify_fd = g.fd_notify;
    fdclose_sock = notify_fd;
    /* the old image may have shut fd passing at the cap; this image starts
     * with fdclose_shut = 0, so fdclose_passrights(1) would skip it */
    if (notify_fd >= 0) {
        int on = 1;
        setsockopt(notify_fd, SOL_SOCKET, SO_PASSRIGHTS, &on, sizeof on);
    }
    watchdog_fd = g.fd_watchdog;
    if (ctl_fd >= 0) fcntl(ctl_fd, F_SETFD, FD_CLOEXEC);
    if (notify_fd >= 0) fcntl(notify_fd, F_SETFD, FD_CLOEXEC);
    if (watchdog_fd >= 0) {
        int timeout = 0;
        fcntl(watchdog_fd, F_SETFD, FD_CLOEXEC);
        if (ioctl(watchdog_fd, WDIOC_GETTIMEOUT, &timeout) < 0) timeout = 0;
        watchdog_pet_ms = wd_pet_ms(timeout);
        write(watchdog_fd, "\0", 1);
    }

    struct timespec now_mono;
    clock_gettime(CLOCK_MONOTONIC, &now_mono);
    for (int i = 0; i < svc_count; i++) {
        int j;
        for (j = 0; j < nrs; j++)
            if (!strcmp(services[i].name, reexec_svcs[j].name)) break;
        if (j == nrs) { fresh++; continue; }
        svc_runtime_copy(&services[i], &reexec_svcs[j].rt);
        svc_clamp_hold(&services[i], &now_mono);
        svc_carry_timer_done(&services[i], !reexec_svcs[j].timer);
        listen_adopt(&services[i]);
        adopted++;
    }
    /* a service gone from the config: its sockets go with it */
    for (int j = 0; j < nrs; j++) {
        int i;
        for (i = 0; i < svc_count; i++)
            if (!strcmp(services[i].name, reexec_svcs[j].name)) break;
        if (i < svc_count) continue;
        for (int k = 0; k < reexec_svcs[j].rt.listen_open && k < MAX_LISTEN; k++)
            close(reexec_svcs[j].rt.listen_fd[k]);
    }
    if (g.argv0[0] == '/')
        snprintf(engine_reexec_argv0, sizeof engine_reexec_argv0, "%s", g.argv0);
    memcpy(evictions, ev, sizeof(eviction_t) * (size_t)nev);
    eviction_count = nev;
    for (int k = 0; k < eviction_count; k++)
        evictions[k].deadline = mono_sec() + (evictions[k].deadline - time(NULL));
    eviction_readded();
    init_start = g.init_start;
    /* an older image wrote no booted_version: it booted as itself, or so we must assume */
    snprintf(booted_version, sizeof booted_version, "%s", g.booted_version[0] ? g.booted_version : g.version);
    snprintf(booted_image, sizeof booted_image, "%s", g.booted_image);
    if (g.nofile_soft)
        service_set_nofile_soft_at_boot((rlim_t)g.nofile_soft);
    system_under_pressure = g.under_pressure;
    engine_reexec_lock_fd = g.fd_lock;
    if (engine_reexec_lock_fd >= 0) fcntl(engine_reexec_lock_fd, F_SETFD, FD_CLOEXEC);
    engine_hooks.reexec_restore_host(&g);

    printf("[%s] re-exec %s -> %s: %d adopted, %d new\n", engine_paths.tag,
           g.version, SCHEMA_INIT_VERSION, adopted, fresh);
    if (g.fd_client >= 0) {
        fcntl(g.fd_client, F_SETFD, FD_CLOEXEC);
        reexec_reply_fd = g.fd_client;
        if (rolled_back)
            snprintf(reexec_reply, sizeof reexec_reply,
                     "err: rolled back to %s, %d services adopted\n",
                     SCHEMA_INIT_VERSION, adopted);
        else
            snprintf(reexec_reply, sizeof reexec_reply,
                     "ok: re-executed into %s, %d services adopted, %d new\n",
                     SCHEMA_INIT_VERSION, adopted, fresh);
    }
    close(blob_fd);
    if (oldexe_fd >= 0) close(oldexe_fd);
}

/* ── re-exec: the old image ─────────────────────────────────────────── */

/* A sealed memfd holding the live state, CLOEXEC: it is opened to the dry run
 * and the new image explicitly, never inherited by a service. */
static int reexec_write_blob(int client, int oldexe, char *err, size_t errsz) {
    reexec_global_t g;
    int fd = memfd_create("schema-init-state", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        snprintf(err, errsz, "memfd_create: %s", strerror(errno));
        return -1;
    }
    memset(&g, 0, sizeof g);
    snprintf(g.version, sizeof g.version, "%s", SCHEMA_INIT_VERSION);
    snprintf(g.argv0, sizeof g.argv0, "%s", engine_reexec_argv0);
    snprintf(g.booted_version, sizeof g.booted_version, "%s", booted_version);
    snprintf(g.booted_image, sizeof g.booted_image, "%s", booted_image);
    g.init_start = init_start;
    g.under_pressure = system_under_pressure;
    g.nofile_soft = (uint64_t)service_nofile_soft_at_boot();
    g.fd_ctl = ctl_fd;
    g.fd_notify = notify_fd;
    g.fd_watchdog = watchdog_fd;
    g.fd_client = client;
    g.fd_oldexe = oldexe;
    g.fd_lock = engine_reexec_lock_fd;
    engine_hooks.reexec_save_host(&g);

    int d = dup(fd);
    FILE *f = d >= 0 ? fdopen(d, "w") : NULL;
    if (!f) {
        snprintf(err, errsz, "state blob: %s", strerror(errno));
        if (d >= 0) close(d);
        close(fd);
        return -1;
    }
    /* the blob keeps wall-clock deadlines, so older and newer images agree */
    eviction_t wall_ev[MAX_EVICTIONS];
    for (int k = 0; k < eviction_count; k++) {
        wall_ev[k] = evictions[k];
        wall_ev[k].deadline = time(NULL) + (evictions[k].deadline - mono_sec());
    }
    state_write(f, &g, services, svc_count, wall_ev, eviction_count, fdstore_v, fdstore_n);
    if (fclose(f) != 0 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0) {
        snprintf(err, errsz, "state blob: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void reexec_reply_end(const char *fmt, const char *arg) {
    if (reexec_client < 0) return;
    ctl_writef(reexec_client, fmt, arg);
    write(reexec_client, ".\n", 2);
    close(reexec_client);
    reexec_client = -1;
}

/* schema-ctl reexec [PATH]: refuse what can be refused now, then start the
 * dry run as a plain child and go back to the main loop. PID 1 never waits
 * on it: a blocked PID 1 starves the watchdog and stops reaping. */
int engine_reexec_start(int client, const char *path, char *err, size_t errsz) {
    struct stat st;
    int out[2], blob;
    if (!engine_running) {
        snprintf(err, errsz, "shutting down");
        return -1;
    }
    if (reexec_check_pid > 0) {
        snprintf(err, errsz, "a re-exec is already in progress");
        return -1;
    }
    if (engine_hooks.reexec_refuse(err, errsz))
        return -1;
    if (path[0] != '/' || strlen(path) >= sizeof reexec_path) {
        snprintf(err, errsz, "'%s' is not an absolute path", path);
        return -1;
    }
    /* One open file for the checks, the dry run and the commit: whatever
     * replaces the path in between, PID 1 becomes the binary that was
     * checked. O_PATH so a FIFO swapped in can't block PID 1 in open(). */
    if ((reexec_target = open(path, O_PATH | O_CLOEXEC)) < 0) {
        snprintf(err, errsz, "open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(reexec_target, &st) < 0 || !S_ISREG(st.st_mode) || !(st.st_mode & 0111)) {
        snprintf(err, errsz, "'%s' is not an executable file", path);
        close(reexec_target); reexec_target = -1;
        return -1;
    }
    if ((blob = reexec_write_blob(-1, -1, err, errsz)) < 0) {
        close(reexec_target); reexec_target = -1;
        return -1;
    }
    if (pipe2(out, O_CLOEXEC) < 0) {
        snprintf(err, errsz, "pipe: %s", strerror(errno));
        close(blob); close(reexec_target); reexec_target = -1;
        return -1;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errsz, "fork: %s", strerror(errno));
        close(blob); close(out[0]); close(out[1]); close(reexec_target); reexec_target = -1;
        return -1;
    }
    if (pid == 0) {
        char b[16];
        char *av[] = { (char *)path, "--reexec-check", b, NULL };
        service_reset_child_sigmask();
        service_restore_child_nofile();
        dup2(out[1], STDOUT_FILENO);
        fcntl(blob, F_SETFD, 0);
        fcntl(reexec_target, F_SETFD, 0);   /* a #! script needs it open */
        snprintf(b, sizeof b, "%d", blob);
        fexecve(reexec_target, av, rx_env());
        dprintf(STDOUT_FILENO, "err: exec %s: %s\n", path, strerror(errno));
        _exit(127);
    }
    close(out[1]);
    close(blob);
    set_nonblock(out[0]);
    reexec_check_out = out[0];
    reexec_check_pid = pid;
    reexec_check_killed = 0;
    reexec_check_due = (int64_t)monotonic_ms() + REEXEC_CHECK_MS;
    reexec_client = fcntl(client, F_DUPFD_CLOEXEC, 3);
    snprintf(reexec_path, sizeof reexec_path, "%s", path);
    printf("[%s] re-exec: dry run of %s (pid %d)\n", engine_paths.tag, path, (int)pid);
    return 0;
}

/* Called from the main loop: a dry run past its deadline gets SIGKILL; reap()
 * then reports it. */
void engine_reexec_tick(void) {
    if (reexec_check_pid > 0 && !reexec_check_killed &&
        (int64_t)monotonic_ms() >= reexec_check_due) {
        kill(reexec_check_pid, SIGKILL);
        reexec_check_killed = 1;
    }
}

/* Re-check what can have changed during the dry run, against a fresh blob:
 * a .svc edited, a new orphan, a cycle. The same checks the dry run made. */
static int reexec_recheck(int blob, char *err, size_t errsz) {
    reexec_global_t g;
    eviction_t ev[MAX_EVICTIONS];
    int nrs, nev, nfs, shadow_count, shadow_grp_count, rc;

    memset(shadow_services, 0, sizeof(service_t) * MAX_SERVICES);
    memset(shadow_groups, 0, sizeof(group_t) * MAX_GROUPS);
    shadow_count = services_load(engine_paths.svc_dir, shadow_services, MAX_SERVICES);
    if (shadow_count == 0 && engine_paths.svc_fallback)
        shadow_count = services_load(engine_paths.svc_fallback, shadow_services, MAX_SERVICES);
    shadow_grp_count = groups_load(engine_paths.svc_dir, shadow_groups, MAX_GROUPS);
    if (shadow_grp_count == 0 && engine_paths.svc_fallback)
        shadow_grp_count = groups_load(engine_paths.svc_fallback, shadow_groups, MAX_GROUPS);

    if (validate_and_resolve(shadow_services, shadow_count, shadow_groups, shadow_grp_count) > 0) {
        snprintf(err, errsz, "dependency cycle in %s", engine_paths.svc_dir);
        rc = -1;
    } else if ((rc = reexec_read_blob(blob, &g, &nrs, ev, &nev, NULL, 0, &nfs, err, errsz)) == 0) {
        rc = reexec_compare(reexec_svcs, nrs, shadow_services, shadow_count, err, errsz);
    }
    for (int k = 0; k < shadow_count; k++)
        for (int m = 1; m < MAX_ARGV; m++)
            if (shadow_services[k].argv[m]) { free(shadow_services[k].argv[m]); shadow_services[k].argv[m] = NULL; }
    for (int k = 0; k < shadow_count; k++) service_free_landlock(&shadow_services[k]);
    return rc;
}

static void set_cloexec(int fd, int on) {
    if (fd >= 0) fcntl(fd, F_SETFD, on ? FD_CLOEXEC : 0);
}

static void listen_set_cloexec(int on) {
    for (int i = 0; i < svc_count; i++)
        for (int k = 0; k < services[i].listen_open; k++)
            set_cloexec(services[i].listen_fd[k], on);
}

static void reexec_commit(void) {
    char err[320];
    sigset_t held, prev;
    int oldexe, blob;

    if (!engine_running) {
        reexec_reply_end("err: %s\n", "shutting down");
        return;
    }
    int q = fdclose_quiesce();
    if (q < 0) {
        reexec_reply_end("err: %s\n", q == -2
            ? "an fd closer outlived SIGKILL (stuck in D state); try again later, or reboot"
            : "an fd closer has not reached close() yet, try again");
        return;
    }
    notify_pause_track();
    oldexe = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);

    /* execve resets our handlers to SIG_DFL and the kernel drops SIG_DFL
     * signals sent to PID 1. Blocked, a reboot sent now stays pending and the
     * new image receives it once its handlers are in. */
    held_signals(&held);
    sigprocmask(SIG_BLOCK, &held, &prev);

    blob = reexec_write_blob(reexec_client, oldexe, err, sizeof err);
    if (blob >= 0 && reexec_recheck(blob, err, sizeof err) == 0) {
        char b[16], o[16];
        int fds[] = { ctl_fd, notify_fd, watchdog_fd, reexec_client, oldexe, blob, engine_reexec_lock_fd };
        for (size_t k = 0; k < sizeof fds / sizeof fds[0]; k++)
            set_cloexec(fds[k], 0);
        listen_set_cloexec(0);
        fdstore_set_cloexec(0);
        fdclose_set_cloexec(0);
        snprintf(b, sizeof b, "%d", blob);
        snprintf(o, sizeof o, "%d", oldexe);
        printf("[%s] re-exec: %s\n", engine_paths.tag, reexec_path);
        ctl_pend_abort(engine_reexec_keep_argv0 ? "schema-user is re-executing, try again"
                                                : "PID 1 is re-executing, try again");
        if (watchdog_fd >= 0)
            write(watchdog_fd, "\0", 1);
        fflush(NULL);
        {
            char *av[] = { engine_reexec_keep_argv0 ? engine_reexec_argv0 : reexec_path, "--reexec", b, o, NULL };
            fexecve(reexec_target, av, rx_env());
        }
        snprintf(err, sizeof err, "exec %s: %s", reexec_path, strerror(errno));
        for (size_t k = 0; k < sizeof fds / sizeof fds[0]; k++)
            set_cloexec(fds[k], 1);
        listen_set_cloexec(1);
        fdstore_set_cloexec(1);
        fdclose_set_cloexec(1);
    }
    sigprocmask(SIG_SETMASK, &prev, NULL);
    if (blob >= 0) close(blob);
    if (oldexe >= 0) close(oldexe);
    /* after the two above: either may hold a number a killed closer freed */
    fdclose_resume();
    close(reexec_target);
    reexec_target = -1;
    printf("[%s] re-exec refused: %s\n", engine_paths.tag, err);
    reexec_reply_end("err: %s\n", err);
}

static void reexec_check_done(int status) {
    char out[512] = "", *line, *nl;
    ssize_t n, len = 0;

    while (len < (ssize_t)sizeof out - 1 &&
           (n = read(reexec_check_out, out + len, sizeof out - 1 - (size_t)len)) > 0)
        len += n;
    out[len] = '\0';
    close(reexec_check_out);
    reexec_check_out = -1;
    reexec_check_pid = 0;

    /* the verdict is the child's last line; the config loader talks first */
    while (len > 0 && out[len - 1] == '\n') out[--len] = '\0';
    line = (nl = strrchr(out, '\n')) ? nl + 1 : out;

    if (reexec_check_killed || WIFSIGNALED(status) || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || strncmp(line, "ok:", 3)) {
        close(reexec_target);
        reexec_target = -1;
    }
    if (reexec_check_killed) {
        reexec_reply_end("err: dry run of %s did not finish in 5 s\n", reexec_path);
    } else if (WIFSIGNALED(status)) {
        char why[96];
        snprintf(why, sizeof why, "dry run died on signal %d", WTERMSIG(status));
        reexec_reply_end("err: %s\n", why);
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || strncmp(line, "ok:", 3)) {
        reexec_reply_end("%s\n", line[0] ? line : "err: dry run failed without a reason");
    } else {
        reexec_commit();
    }
}


int engine_reexec_reaped(pid_t pid, int status) {
    if (pid != reexec_check_pid)
        return 0;
    reexec_check_done(status);
    return 1;
}

/* After the adopting image's handlers and timers are in: a stop sent during
 * the swap, held pending since the old image blocked it, arrives now. */
void engine_reexec_finish(void) {
    sigset_t held;
    held_signals(&held);
    sigprocmask(SIG_UNBLOCK, &held, NULL);
    if (reexec_reply_fd >= 0) {
        ctl_writef(reexec_reply_fd, "%s", reexec_reply);
        write(reexec_reply_fd, ".\n", 2);
        close(reexec_reply_fd);
        reexec_reply_fd = -1;
    }
}

