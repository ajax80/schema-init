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
#include "svc_dropins.h"
#include "reexec_state.h"
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
int          system_under_pressure = 0;
static int          pass_changed = 1;   /* first pass, or last pass moved a service: run at once */
#define PASS_RERUN_MAX  16                /* ...at most this many passes in a row */
int          psi_fd = -1;
static int          ino_fd = -1;
static int64_t      ready_event_due = 0;    /* CLOCK_MONOTONIC ms, 0 = none */
static int64_t      ready_backstop_due = 0;
int64_t      cpu_check_due = 0;
struct timespec init_start;

static void start_failsafe(service_t *svc);
static void active_kill_service(service_t *svc);
static int handle_reload(int evict_mode, char *err, size_t errsz);
pid_t   reexec_check_pid = 0;
int64_t reexec_check_due = 0;
int     reexec_check_killed = 0;

/* Arm a calendar timer: set timer_next to the next CLOCK_REALTIME instant at
 * which local wall-clock reaches HH:MM on a day matching the timer's dow/dom
 * constraints, strictly in the future. Walks forward day-by-day (mktime per
 * step re-normalizes wday/mday and self-corrects DST/NTP each cycle). Daily
 * timers (dow=dom=-1) match the first or second day and behave as before. */
static void timer_arm_calendar(service_t *svc) {
    svc->timer_next.tv_sec  = calendar_next_after(svc->timer_cal_hour,
                                                  svc->timer_cal_min,
                                                  svc->timer_cal_dow,
                                                  svc->timer_cal_dom, time(NULL));
    svc->timer_next.tv_nsec = 0;
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

/* most recent past (or current) occurrence of the timer's HH:MM on a day
 * matching its dow/dom constraints, <= now. Walks backward day-by-day; returns
 * 0 if none in the last ~400 days (constraint never satisfiable). */
static time_t calendar_recent_occurrence(const service_t *svc, time_t now) {
    return calendar_recent_at_or_before(svc->timer_cal_hour, svc->timer_cal_min,
                                        svc->timer_cal_dow, svc->timer_cal_dom, now);
}

/* Initial arm for a persistent calendar timer: if a scheduled fire passed while
 * we were down, fire now; otherwise arm normally. A never-stamped timer is
 * seeded (no replay of history on first deploy). */
static void timer_arm_persistent(service_t *svc) {
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
        service_log(svc, "timer-catchup");
    } else {
        timer_arm_calendar(svc);
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

static void eviction_drop(int i) {
    if (evictions[i].cgroup[0]) rmdir(evictions[i].cgroup);
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
        if (!killed && !evictions[i].reaped) {
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

static void fallback_up(service_t *fb) {
    fb->standby_up = 1;
    fb->flags &= ~SVC_NO_RESTART;
    if (fb->child_pid <= 0 && (fb->inst.state == STATE_EXCISED || fb->inst.state == STATE_PERFECT)) {
        fb->inst.state = STATE_NEW_PROCESS;
        fb->restart_count = 0;
    }
    service_log(fb, "fallback-up");
}

static void fallback_down(service_t *fb) {
    if (!fb->standby_up) return;
    fb->standby_up = 0;
    fb->flags |= SVC_NO_RESTART;
    if (fb->child_pid > 0) {
        fb->ctl_killed = 1;
        kill(fb->child_pid, SIGTERM);
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

static int held_now(const service_t *svc) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return svc_retry_held(svc, &now);
}

static void ctl_rearm(service_t *svc, int start) {
    svc->card_next = 0;
    svc->restart_at.tv_sec = svc->restart_at.tv_nsec = 0;
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
    if (pid < 0) {
        service_log(svc, "card-fork-fail");
        return;
    }
    svc->card_pid = pid;
    clock_gettime(CLOCK_MONOTONIC, &svc->card_start);
    service_log(svc, ev);
}

static void card_done(service_t *svc, int status) {
    svc->card_pid = 0;
    service_log(svc, WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "card-done" : "card-failed");
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
    char path[320], buf[32];
    struct stat st;
    FILE *f;
    long p = 0, q;
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
    snprintf(path, sizeof path, "%s/cgroup.procs", svc->cgroup_path);
    if (!(f = fopen(path, "r"))) return 0;
    while (!found && fscanf(f, "%ld", &q) == 1) found = q == p;
    fclose(f);
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

/* timer fire complete: re-arm regardless of exit code (cron semantics — a
 * failed or condition-skipped run isn't retried, it runs next window).
 * interval 0 = run-once (only on_boot_sec): drop the flag so PERFECT becomes
 * terminal instead of re-firing every tick. */
static void timer_rearm(service_t *svc) {
    svc->inst.state = STATE_PERFECT;
    if (svc->flags & SVC_TIMER_CALENDAR) {
        if (svc->flags & SVC_TIMER_PERSIST)
            timer_stamp_write(svc, time(NULL));
        timer_arm_calendar(svc);
    } else if (svc->timer_interval_sec > 0) {
        struct timespec tn;
        clock_gettime(CLOCK_MONOTONIC, &tn);
        svc->timer_next = tn;
        svc->timer_next.tv_sec += svc->timer_interval_sec;
    } else {
        svc->flags &= ~SVC_TIMER;
    }
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
        if (engine_hooks.on_reaped(pid, status))
            continue;
        if (fdclose_reaped(pid)) continue;
        for (i = 0; i < svc_count; i++) {
            if (services[i].card_pid == pid) {
                card_done(&services[i], status);
                break;
            }
            if (services[i].child_pid != pid) continue;

            services[i].last_pid   = pid;
            services[i].child_pid  = 0;
            note_exit(&services[i], status);

            if (services[i].flags & SVC_TIMER) {
                timer_rearm(&services[i]);
                service_log(&services[i],
                    services[i].exit_status == 0 ? "timer-done" : "timer-failed");
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
                    service_log(&services[i], "pid_file-no-cgroup");
                    break;
                }
                fork_adopt(&services[i]);
                break;
            }

            if (services[i].listen_open && !services[i].ctl_killed &&
                !(services[i].flags & SVC_ONESHOT) &&
                ((WIFEXITED(status) && WEXITSTATUS(status) == 0) ||
                 (services[i].flags & SVC_NO_RESTART)) &&
                mono_sec() - services[i].spawn_time_mono.tv_sec >= LISTEN_MIN_RUN_SECS) {
                /* idle exit of a socket service: PID 1 still holds its
                 * sockets, the next connection starts it again */
                services[i].inst.state = STATE_PERFECT;
                clock_gettime(CLOCK_MONOTONIC, &services[i].stable_time);
                service_log(&services[i], "socket-idle");
            } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0
                && (services[i].flags & SVC_ONESHOT)) {
                /* clean one-shot exit → PERFECT */
                services[i].inst.state = STATE_PERFECT;
                clock_gettime(CLOCK_MONOTONIC, &services[i].stable_time);
                mark_boot_ready(&services[i], READY_EXIT);
                service_log(&services[i], "oneshot-done");
                if (strcmp(services[i].name, "slot-detect") == 0) {
                    load_env_file();
                }
            } else if (services[i].flags & SVC_NO_RESTART) {
                services[i].inst.state = STATE_EXCISED;
                service_log(&services[i], "76-no-restart");
                /* Explicit stop/down lands here straight from reap() — it
                 * never crosses the FRICTION→EXCISED transition that the tick
                 * loop uses to tear cgroups down, and case STATE_EXCISED is a
                 * no-op. Without this an isolated cpuset partition keeps its
                 * core carved out of general scheduling (and leaks the parent
                 * cpuset.cpus.exclusive union) until reboot. Release here so
                 * stop frees the reservation; cgroup_assign() rebuilds the
                 * cgroup on any later start. */
                service_cgroup_kill(&services[i]);
            } else if (services[i].ctl_killed) {
                if (services[i].pid_file[0]) service_cgroup_kill(&services[i]);
                services[i].inst.state = STATE_NEW_PROCESS;
                service_log(&services[i], "ctl-restart");
            } else {
                /* unexpected death → enter recovery arc */
                restart_budget_refresh(&services[i]);
                if (services[i].failsafe_cmd[0]) {
                    start_failsafe(&services[i]);
                }
                services[i].inst.state = STATE_RECOVERY;
                service_log(&services[i], "died");
            }
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
        if (svc->card_pid > 0 && now.tv_sec - svc->card_start.tv_sec >= CARD_TIMEOUT_SECS) {
            int st;
            kill(-svc->card_pid, SIGKILL);
            kill(svc->card_pid, SIGKILL);
            if (waitpid(svc->card_pid, &st, 0) == svc->card_pid || errno == ECHILD) {
                svc->card_pid = 0;
                service_log(svc, "card-timeout");
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
    
    kill(svc->child_pid, SIGTERM);
    
    int elapsed_ms = 0;
    int status;
    while (elapsed_ms < 50) {
        pid_t res = waitpid(svc->child_pid, &status, WNOHANG);
        if (res == svc->child_pid) {
            note_exit(svc, status);
            svc->child_pid = 0;
            service_cgroup_kill(svc);
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
    service_cgroup_kill(svc);
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
static int svc_held_on_socket(const service_t *svc) {
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
    if (svc->flags & SVC_NO_RESTART) {
        svc->inst.state = STATE_EXCISED;
        service_log(svc, "76-no-restart");
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

static void ready_inotify_drain(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (read(ino_fd, buf, sizeof(buf)) > 0)
        ;
    if (!ready_event_due)
        ready_event_due = (int64_t)monotonic_ms() + READY_EVENT_GRACE_MS;
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
                service_log(svc, "fuse-tripped");
                if (svc->child_pid > 0) {
                    kill(svc->child_pid, SIGKILL);
                    service_cgroup_kill(svc);
                    svc->child_pid = 0;
                }
                break;
            }
        }
    }

    switch (svc->inst.state) {

        case STATE_NEW_PROCESS:
            if (svc->failsafe_pid > 0 || svc->card_pid > 0) break;
            if (svc_retry_held(svc, &now_mono)) break;
            if (!svc->standby_up && service_is_standby(svc, services, svc_count)) {
                svc->flags |= SVC_NO_RESTART;
                svc->inst.state = STATE_EXCISED;
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
            if (svc->inst.state == STATE_FULL_TRUST) {
                service_log(svc, "spawn");
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
                    service_cgroup_kill(svc);
                    restart_budget_refresh(svc);
                    if (svc->failsafe_cmd[0]) start_failsafe(svc);
                    svc->inst.state = STATE_RECOVERY;
                    service_log(svc, "pid_file-missing");
                }
                break;
            }
            if (!(svc->flags & SVC_ONESHOT) && svc->child_pid > 0) {
                int ready = 0;
                if (svc->pid_file[0] && svc->fork_state != 1) {
                    /* not before the launcher has exited and the daemon is adopted */
                } else if ((svc->notify || svc->ready_bus_name[0]) && svc->notify_ready) {
                    ready = svc->ready_bus_name[0] ? READY_BUS : READY_NOTIFY;
                } else if (svc->ready_path[0] && ready_path_fresh(svc)) {
                    ready = READY_PATH;
                    svc->ready_path_verified = 1;
                } else if (svc->pid_file[0]) {
                    if (svc->fork_state == 1) ready = READY_PIDFILE;
                } else if (now_mono.tv_sec - svc->spawn_time_mono.tv_sec >= svc->stable_secs) {
                    ready = READY_TIMER;
                }
                if (ready) {
                    flags = service_probe_f8(svc, services, svc_count);
                    schema_step(&svc->inst, flags);
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
                service_cgroup_kill(svc);
                svc->dormant_count++;
                if (!(svc->flags & SVC_CRITICAL) && !svc->no_excise && svc->dormant_count > 4) {
                    service_log(svc, "76-excised");
                } else {
                    svc->dormant_count--;
                    park(svc);
                }
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

static void shm_update(void) {
    int i;
    if (!shm_ptr) return;
    for (i = 0; i < svc_count; i++) {
        memcpy(shm_ptr->svc[i].name, services[i].name, 64);
        shm_ptr->svc[i].state         = services[i].inst.state;
        shm_ptr->svc[i].weight        = services[i].inst.weight;
        shm_ptr->svc[i].child_pid     = (int32_t)services[i].child_pid;
        shm_ptr->svc[i].restart_count = services[i].restart_count;
        shm_ptr->svc[i].card_played   = (uint8_t)services[i].card_next;
        shm_ptr->svc[i].card_count    = (uint8_t)services[i].card_count;
        service_t *fb = services[i].fallback_live ? service_fallback(&services[i], services, svc_count) : NULL;
        shm_ptr->svc[i].limp = (fb && fb->standby_up ? SHM_LIMP_FALLBACK : 0)
            | (service_is_standby(&services[i], services, svc_count)
               ? (services[i].standby_up ? SHM_LIMP_STANDIN : SHM_LIMP_STANDBY) : 0);
    }
    shm_ptr->count = svc_count;
    for (i = 0; i < grp_count; i++) {
        memcpy(shm_ptr->groups[i].name, groups[i].name, 64);
        shm_ptr->groups[i].state        = groups[i].state;
        shm_ptr->groups[i].member_count = groups[i].member_count;
    }
    shm_ptr->group_count = grp_count;
    shm_ptr->seq++;
}

/* ── runtime control socket ─────────────────────────────────────────── */

void ctl_writef(int fd, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) write(fd, buf, (size_t)n);
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

static const char *hard_source(const service_t *s, uint8_t bit) {
    if (s->hard_set & bit) return "explicit";
    if (s->hard_dropped & bit) return "dropped (default would hide exec/ready_path)";
    if (s->hard_default & bit) return "default";
    return "unset";
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

static void ctl_cmd(int fd, char *line) {
    int i, extra;
    line[strcspn(line, "\r\n")] = '\0';

    if (strncmp(line, "status", 6) == 0) {
        char *opt = line + 6;
        while (*opt == ' ') opt++;
        if (strcmp(opt, "--json") == 0) {
            ctl_writef(fd, "{\n  \"services_count\": %d,\n  \"groups_count\": %d,\n  \"services\": [\n", svc_count, grp_count);
            for (i = 0; i < svc_count; i++) {
                ctl_writef(fd, "    {\n      \"name\": \"%s\",\n      \"pid\": %d,\n      \"state\": \"%s\",\n      \"restarts\": %d\n    }%s\n",
                    services[i].name, (int)services[i].child_pid,
                    state_name(services[i].inst.state), services[i].restart_count,
                    (i < svc_count - 1) ? "," : "");
            }
            ctl_writef(fd, "  ]\n}\n");
        } else if (strcmp(opt, "--kv") == 0) {
            ctl_writef(fd, "services_count=%d\ngroups_count=%d\n", svc_count, grp_count);
            for (i = 0; i < svc_count; i++) {
                ctl_writef(fd, "service.%s.pid=%d\n", services[i].name, (int)services[i].child_pid);
                ctl_writef(fd, "service.%s.state=%s\n", services[i].name, state_name(services[i].inst.state));
                ctl_writef(fd, "service.%s.restarts=%d\n", services[i].name, services[i].restart_count);
                ctl_writef(fd, "service.%s.last_exit=%s\n", services[i].name, last_exit_str(&services[i]));
                if (svc_held_on_socket(&services[i]))
                    ctl_writef(fd, "service.%s.waiting=%s\n", services[i].name, services[i].wait_socket);
            }
        } else if (*opt) {
            for (i = 0; i < svc_count && strcmp(services[i].name, opt) != 0; i++) ;
            if (i == svc_count) {
                ctl_writef(fd, "err: not found: %s\n", opt);
            } else {
                const service_t *s = &services[i];
                static const char *const ps[] = { "0", "1", "full" };
                ctl_writef(fd, "%s  pid=%d  state=%s  restarts=%d\n", s->name,
                    (int)s->child_pid, state_name(s->inst.state), s->restart_count);
                ctl_writef(fd, "last exit: %s\n", last_exit_str(s));
                ctl_writef(fd, "hardening default: %s\n", service_hardening_default() ? "ON" : "off");
                ctl_writef(fd, "  %-15s %-5s %s\n", "no_new_privs",
                    (s->flags & SVC_NO_NEW_PRIVS) ? "1" : "0", hard_source(s, HARD_NNP));
                ctl_writef(fd, "  %-15s %-5d %s\n", "private_tmp", s->ns_private_tmp, hard_source(s, HARD_PT));
                ctl_writef(fd, "  %-15s %-5s %s\n", "protect_system",
                    ps[s->ns_protect_system < 3 ? s->ns_protect_system : 0], hard_source(s, HARD_PS));
                ctl_writef(fd, "  %-15s %-5d %s\n", "protect_home", s->ns_protect_home, hard_source(s, HARD_PH));
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
                char deck[48] = "";
                if (services[i].card_count)
                    snprintf(deck, sizeof deck, "  cards=%d/%d", services[i].card_next, services[i].card_count);
                int sk = services[i].fdstore_max > 0 ? fdstore_count(services[i].name) : 0;
                if (sk > 0)
                    snprintf(deck + strlen(deck), sizeof deck - strlen(deck), "  fds=%d", sk);
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
            write(fd, ".\n", 2);
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
                services[i].flags       &= ~SVC_NO_RESTART;
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
            write(fd, ".\n", 2);
            return;
        }
        ctl_writef(fd, "err: not found: %s\n", name);

    } else if (strncmp(line, "stop ", 5) == 0 || strncmp(line, "down ", 5) == 0) {
        const char *name = line + 5;
        for (i = 0; i < svc_count; i++) {
            if (strcmp(services[i].name, name) != 0) continue;
            services[i].flags |= SVC_NO_RESTART;
            services[i].restart_at.tv_sec = services[i].restart_at.tv_nsec = 0;
            if (services[i].child_pid > 0) {
                services[i].ctl_killed = 1;
                kill(services[i].child_pid, SIGTERM);
                ctl_writef(fd, "ok: SIGTERM → %s (pid %d)\n",
                    name, (int)services[i].child_pid);
            } else {
                if (services[i].fork_state < 0) {
                    service_cgroup_kill(&services[i]);
                    services[i].fork_state = 0;
                }
                services[i].inst.state = STATE_EXCISED;
                ctl_writef(fd, "ok: %s stopped (was not running)\n", name);
            }
            write(fd, ".\n", 2);
            return;
        }
        ctl_writef(fd, "err: not found: %s\n", name);

    } else if (strncmp(line, "restart ", 8) == 0) {
        const char *name = line + 8;
        for (i = 0; i < svc_count; i++) {
            if (strcmp(services[i].name, name) != 0) continue;
            services[i].flags &= ~SVC_NO_RESTART;
            services[i].listen_hold = 0;
            if (services[i].child_pid > 0) {
                services[i].ctl_killed = 1;
                kill(services[i].child_pid, SIGTERM);
                ctl_writef(fd, "ok: SIGTERM → %s — recovery arc will respawn\n", name);
            } else {
                if (services[i].fork_state < 0) {
                    service_cgroup_kill(&services[i]);
                    services[i].fork_state = 0;
                }
                services[i].inst.state    = STATE_NEW_PROCESS;
                services[i].restart_count = 0;
                ctl_rearm(&services[i], 1);
                ctl_writef(fd, "ok: %s requeued\n", name);
            }
            write(fd, ".\n", 2);
            return;
        }
        ctl_writef(fd, "err: not found: %s\n", name);

    } else if (strncmp(line, "reset", 5) == 0) {
        const char *name = line + 5;
        while (*name == ' ') name++;
        if (*name == '\0') {
            for (i = 0; i < svc_count; i++) {
                services[i].restart_count = 0;
                services[i].dormant_count = 0;
                ctl_rearm(&services[i], 0);
                services[i].flags &= ~SVC_NO_RESTART;
                if (services[i].inst.state == STATE_EXCISED ||
                    services[i].inst.state == STATE_DORMANT ||
                    services[i].inst.state == STATE_PERFECT) {
                    services[i].inst.state = STATE_NEW_PROCESS;
                }
            }
            ctl_writef(fd, "ok: reset all services\n");
        } else {
            for (i = 0; i < svc_count; i++) {
                if (strcmp(services[i].name, name) != 0) continue;
                services[i].restart_count = 0;
                services[i].dormant_count = 0;
                ctl_rearm(&services[i], 0);
                services[i].flags &= ~SVC_NO_RESTART;
                if (services[i].inst.state == STATE_EXCISED ||
                    services[i].inst.state == STATE_DORMANT ||
                    services[i].inst.state == STATE_PERFECT) {
                    services[i].inst.state = STATE_NEW_PROCESS;
                    ctl_writef(fd, "ok: reset and queued %s\n", name);
                } else {
                    ctl_writef(fd, "ok: reset restart count for %s (current state: %s)\n",
                               name, state_name(services[i].inst.state));
                }
                write(fd, ".\n", 2);
                return;
            }
            ctl_writef(fd, "err: not found: %s\n", name);
        }
        write(fd, ".\n", 2);
        return;


    } else if (strncmp(line, "add ", 4) == 0) {
        const char *path = line + 4;
        int j, k;
        if (svc_count >= MAX_SERVICES) {
            ctl_writef(fd, "err: MAX_SERVICES (%d) reached\n", MAX_SERVICES);
            write(fd, ".\n", 2);
            return;
        }
        if (service_load_one(path, &services[svc_count]) < 0) {
            ctl_writef(fd, "err: failed to load %s\n", path);
            write(fd, ".\n", 2);
            return;
        }
        for (j = 0; j < svc_count; j++) {
            if (strcmp(services[j].name, services[svc_count].name) == 0) {
                ctl_writef(fd, "err: '%s' already loaded\n", services[svc_count].name);
                for (k = 1; k < MAX_ARGV; k++)
                    if (services[svc_count].argv[k]) { free(services[svc_count].argv[k]); services[svc_count].argv[k] = NULL; }
                service_free_landlock(&services[svc_count]);
                write(fd, ".\n", 2);
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
            write(fd, ".\n", 2);
            return;
        }
        /* Arm timers added at runtime, mirroring the boot/reload arm sites.
         * Without this the timer keeps timer_next=0 (a past CLOCK_REALTIME
         * instant) and fires once immediately on the next tick. */
        if (services[svc_count].flags & SVC_TIMER) {
            services[svc_count].inst.state = STATE_PERFECT;
            if (services[svc_count].flags & SVC_TIMER_CALENDAR) {
                if (services[svc_count].flags & SVC_TIMER_PERSIST)
                    timer_arm_persistent(&services[svc_count]);
                else
                    timer_arm_calendar(&services[svc_count]);
            } else {
                struct timespec tnow;
                clock_gettime(CLOCK_MONOTONIC, &tnow);
                services[svc_count].timer_next = tnow;
                services[svc_count].timer_next.tv_sec += services[svc_count].timer_boot_sec;
            }
        }
        ctl_writef(fd, "ok: %s queued\n", services[svc_count].name);
        svc_count++;
        write(fd, ".\n", 2);
        return;

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
    write(fd, ".\n", 2);
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
            if (!(svc->notify || stores) || svc->child_pid <= 0) continue;
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
            if (m.status[0])
                snprintf(svc->notify_status, sizeof svc->notify_status, "%s", m.status);
            if (m.watchdog)
                clock_gettime(CLOCK_MONOTONIC, &svc->last_pet);
            if (m.ready && !svc->notify_ready) {
                svc->notify_ready = 1;
                clock_gettime(CLOCK_MONOTONIC, &svc->last_pet);
                service_log(svc, "notify-ready");
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

static void ctl_poll(void) {
    char buf[256];
    int pos = 0, cfd;
    ssize_t n;
    struct timeval tv = {0, 100000}; /* 100ms receive timeout */
    if (ctl_fd < 0) return;
    cfd = accept4(ctl_fd, NULL, NULL, SOCK_CLOEXEC);
    if (cfd < 0) return;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Peer identity: root and our own uid may run everything; schema-group
     * members (allowed in by the 0660 socket) get read-only verbs only. */
    struct ucred cred;
    socklen_t clen = sizeof(cred);
    uid_t peer_uid = (uid_t)-1;
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0)
        peer_uid = cred.uid;

    while (pos < (int)sizeof(buf) - 1) {
        n = recv(cfd, buf + pos, 1, 0);
        if (n <= 0) break;
        if (buf[pos++] == '\n') break;
    }
    if (pos > 0) {
        buf[pos] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';
        if (peer_uid != 0 && peer_uid != geteuid() && !ctl_is_readonly(buf)) {
            ctl_writef(cfd, "err: '%s' requires root (uid %d has read-only access)\n",
                       buf, (int)peer_uid);
            write(cfd, ".\n", 2);
        } else {
            ctl_cmd(cfd, buf);
        }
    }
    close(cfd);
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
        if (svc->failsafe_pid > 0 || svc->card_pid > 0 || (svc->watchdog_timeout_ms > 0 && svc->child_pid > 0))
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
    if (watched)
        best = wake_min(best, ready_backstop_due, now_m, WAKE_NONE);
    if (ready_event_due)
        best = wake_min(best, ready_event_due, now_m, WAKE_NONE);
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
                    if (eviction_count < MAX_EVICTIONS) {
                        evictions[eviction_count].pid      = services[j].child_pid;
                        evictions[eviction_count].deadline = mono_sec() + EVICT_GRACE_SECS;
                        evictions[eviction_count].reaped   = 0;
                        evictions[eviction_count].killed   = 0;
                        memcpy(evictions[eviction_count].cgroup,
                               services[j].cgroup_path,
                               sizeof(evictions[eviction_count].cgroup));
                        eviction_count++;
                    }
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
    eviction_readded();

    /* Arm newly introduced timer services or ensure they are scheduled */
    struct timespec tnow;
    clock_gettime(CLOCK_MONOTONIC, &tnow);
    for (i = 0; i < svc_count; i++) {
        if (services[i].flags & SVC_TIMER) {
            if (services[i].timer_next.tv_sec == 0) {
                services[i].inst.state = STATE_PERFECT;
                if (services[i].flags & SVC_TIMER_CALENDAR) {
                    if (services[i].flags & SVC_TIMER_PERSIST)
                        timer_arm_persistent(&services[i]);
                    else
                        timer_arm_calendar(&services[i]);
                } else {
                    services[i].timer_next = tnow;
                    services[i].timer_next.tv_sec += services[i].timer_boot_sec;
                }
            }
        }
    }
    return 0;
}

int engine_reload(int evict, char *err, size_t errsz) {
    return handle_reload(evict, err, errsz);
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
        for (i = 0; i < svc_count; i++)
            if (services[i].child_pid == pid) { services[i].child_pid = 0; break; }
    }
}

/* Stop in reverse dependency order: a service gets SIGTERM once nothing still
 * running depends on it, then has SHUT_GRACE_MS (or its stop_timeout_sec) to
 * exit before its cgroup is killed. A straggler holds back only what it depends on. Past SHUT_TOTAL_MS
 * everything left gets SIGTERM at once. stop_first services go before all of it. */
static void shutdown_ordered(void) {
    uint64_t start = monotonic_ms(), term_at[MAX_SERVICES] = {0};
    int alive[MAX_SERVICES], killed[MAX_SERVICES] = {0}, gone[MAX_SERVICES] = {0};
    int i, j, round = 0, held = 0;
    char msg[320];

    for (;;) {
        uint64_t now = monotonic_ms();
        int left = 0, sent = 0;
        size_t off = 0;

        shut_reap();
        for (i = 0; i < svc_count; i++) {
            alive[i] = !killed[i] && svc_signal_all(&services[i], 0) > 0;
            left += alive[i];
            if (!alive[i] && term_at[i] && !killed[i] && !gone[i]) {
                gone[i] = 1;
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
                uint64_t grace = services[i].stop_timeout_sec ? (uint64_t)services[i].stop_timeout_sec * 1000
                                                         : SHUT_GRACE_MS;
                if (now - term_at[i] < grace) continue;
                snprintf(msg, sizeof msg, "%s ignored SIGTERM for %llu ms, killing",
                         services[i].name, (unsigned long long)grace);
                engine_hooks.shut_log(msg);
                service_cgroup_kill(&services[i]);
                if (services[i].child_pid > 0) kill(services[i].child_pid, SIGKILL);
                killed[i] = 1;
                continue;
            }
            if (hold && !(services[i].flags & SVC_STOP_FIRST)) continue;
            if (now - start < SHUT_TOTAL_MS)
                for (j = 0; j < svc_count && !blocked; j++)
                    blocked = j != i && alive[j] && shut_depends_on(&services[j], i);
            if (blocked) continue;
            svc_signal_all(&services[i], SIGTERM);
            term_at[i] = now;
            if (!sent++)
                off = snprintf(msg, sizeof msg, "stop %d (+%llu ms):", ++round,
                               (unsigned long long)(now - start));
            if (off < sizeof msg)
                off += snprintf(msg + off, sizeof msg - off, " %s", services[i].name);
        }
        if (sent) engine_hooks.shut_log(msg);
        usleep(20000);
    }
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
    if (adopting) {
        /* Children that exited during the swap are zombies only the restored
         * child_pid can attribute; their SIGCHLD stays pending for signalfd. */
        reap();
        ready_event_due = (int64_t)monotonic_ms();
    }
    signalfd_init();
}

/* arm timer services: born "already ran", first fire = boot + on_boot_sec.
 * After a re-exec only timers new to the config (timer_next still 0). */
void engine_arm_timers(int adopting) {
    struct timespec tnow;
    clock_gettime(CLOCK_MONOTONIC, &tnow);
    for (int ti = 0; ti < svc_count; ti++) {
        if ((services[ti].flags & SVC_TIMER) &&
            (!adopting || services[ti].timer_next.tv_sec == 0)) {
            services[ti].inst.state = STATE_PERFECT;
            if (services[ti].flags & SVC_TIMER_CALENDAR) {
                if (services[ti].flags & SVC_TIMER_PERSIST)
                    timer_arm_persistent(&services[ti]);
                else
                    timer_arm_calendar(&services[ti]);
            } else {
                services[ti].timer_next = tnow;
                services[ti].timer_next.tv_sec += services[ti].timer_boot_sec;
            }
        }
    }
}

void engine_run(void) {
    int i;

    while (engine_running) {
        struct pollfd fds[5 + MAX_SERVICES * MAX_LISTEN];
        int fd_svc[5 + MAX_SERVICES * MAX_LISTEN];
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
                for (e = 0; e < nfixed; e++) {
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
                    }
                }
            } else if (ret < 0 && errno != EINTR) {
                usleep(TICK_USEC);
            }
        } else {
            usleep(TICK_USEC);
        }

        engine_hooks.loop_pressure(psi_fired);

        {
            int64_t now = (int64_t)monotonic_ms();
            if ((ready_event_due && now >= ready_event_due) || now >= ready_backstop_due) {
                ready_event_due = 0;
                ready_backstop_due = now + READY_BACKSTOP_MS;
                ready_recheck_all();
            }
        }

        for (i = 0; i < grp_count; i++)  grp_states[i] = groups[i].state;
        for (i = 0; i < svc_count; i++) {
            uint8_t s = services[i].inst.state;
            if ((services[i].flags & SVC_TIMER) && (s == STATE_NEW_PROCESS || s == STATE_FULL_TRUST)) {
                svc_states[i] = STATE_PERFECT;
            } else {
                svc_states[i] = s;
            }
        }
        uint8_t before[MAX_SERVICES];
        for (i = 0; i < svc_count; i++)
            before[i] = services[i].inst.state;
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
        service_watchdog_tick();
        engine_hooks.loop_tail();
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
        service_cgroup_kill(&services[i]);
    }
}

void engine_release(void) {
    if (notify_fd >= 0) {
        close(notify_fd);
        unlink(engine_paths.notify_sock);
    }
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
