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
#include "service.h"
#include "group.h"
#include "schema_shm.h"
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

#define SVC_DIR         "/etc/schema-init/services"
#define TICK_USEC       250000   /* 250ms main loop tick */
#define CTL_SOCK_PATH   "/run/schema-init.sock"
#define MAX_MOUNTS      128      /* shutdown remount-ro sweep */

static service_t    services_a[MAX_SERVICES];
static service_t    services_b[MAX_SERVICES];
static service_t   *services = services_a;
static service_t   *shadow_services = services_b;
static int          svc_count = 0;

static group_t      groups_a[MAX_GROUPS];
static group_t      groups_b[MAX_GROUPS];
static group_t     *groups = groups_a;
static group_t     *shadow_groups = groups_b;
static int          grp_count = 0;
#define EVICT_GRACE_SECS  3
#define MAX_EVICTIONS     16

typedef struct {
    pid_t  pid;
    char   cgroup[128];
    time_t deadline;
} eviction_t;

static eviction_t evictions[MAX_EVICTIONS];
static int        eviction_count = 0;

static volatile int running   = 1;
static volatile int do_reboot = 0;
static schema_shm_t *shm_ptr = NULL;
static int          ctl_fd   = -1;
static int          sig_fd   = -1;
static int          watchdog_fd = -1;
static int          system_under_pressure = 0;
static int          pressure_clear_ticks = 0;
static struct timespec init_start;

static void start_failsafe(service_t *svc);
static void active_kill_service(service_t *svc);
static int handle_reload(int evict_mode, char *err, size_t errsz);
static int validate_and_resolve(service_t *svc_table, int s_count, group_t *grp_table, int g_count);

/* Arm a calendar timer: set timer_next to the next CLOCK_REALTIME instant at
 * which local wall-clock reaches HH:MM, strictly in the future. Recomputed on
 * every (re-)arm so DST and NTP clock steps self-correct each cycle. */
static void timer_arm_calendar(service_t *svc) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = svc->timer_cal_hour;
    tm.tm_min  = svc->timer_cal_min;
    tm.tm_sec  = 0;
    tm.tm_isdst = -1;
    time_t target = mktime(&tm);
    if (target == (time_t)-1 || target <= now) {
        localtime_r(&now, &tm);
        tm.tm_mday += 1;
        tm.tm_hour = svc->timer_cal_hour;
        tm.tm_min  = svc->timer_cal_min;
        tm.tm_sec  = 0;
        tm.tm_isdst = -1;
        target = mktime(&tm);
    }
    svc->timer_next.tv_sec  = target;
    svc->timer_next.tv_nsec = 0;
}

/* ── persistent calendar timers (persistent=1): catch up a fire missed while
 * the machine was down. Last run is stamped to /var/lib/schema-init/timers/. ── */
#define TIMER_STAMP_DIR "/var/lib/schema-init/timers"

static void timer_stamp_path(const service_t *svc, char *buf, size_t n) {
    snprintf(buf, n, "%s/%s.stamp", TIMER_STAMP_DIR, svc->name);
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
    mkdir("/var/lib", 0755);
    mkdir("/var/lib/schema-init", 0755);
    mkdir(TIMER_STAMP_DIR, 0755);
    char path[256]; timer_stamp_path(svc, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%lld\n", (long long)t);
    fclose(f);
}

/* most recent past (or current) occurrence of HH:MM, <= now */
static time_t calendar_recent_occurrence(const service_t *svc, time_t now) {
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = svc->timer_cal_hour;
    tm.tm_min  = svc->timer_cal_min;
    tm.tm_sec  = 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t > now) {                       /* today's slot is still ahead → yesterday's */
        localtime_r(&now, &tm);
        tm.tm_mday -= 1;
        tm.tm_hour = svc->timer_cal_hour;
        tm.tm_min  = svc->timer_cal_min;
        tm.tm_sec  = 0;
        tm.tm_isdst = -1;
        t = mktime(&tm);
    }
    return t;
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

static void eviction_tick(void) {
    time_t now = time(NULL);
    int i = 0;
    while (i < eviction_count) {
        if (now < evictions[i].deadline) { i++; continue; }
        int killed = 0;
        if (evictions[i].cgroup[0]) {
            char path[160];
            int fd;
            snprintf(path, sizeof(path), "%s/cgroup.kill", evictions[i].cgroup);
            fd = open(path, O_WRONLY);
            if (fd >= 0) { write(fd, "1", 1); close(fd); killed = 1; }
        }
        /* no cgroup, or cgroup.kill unavailable: fall back to a direct SIGKILL.
         * previously this whole branch was gated on cgroup[0], so an orphan
         * with no cgroup that ignored SIGTERM survived eviction entirely. */
        if (!killed) {
            kill(evictions[i].pid, SIGKILL);
        }
        printf("[schema-init] eviction grace expired: force-killed pid=%d\n",
               (int)evictions[i].pid);
        evictions[i] = evictions[--eviction_count];
    }
}

/*
 * IEC 62304 / ISO 26262 Safety Audit Traceability:
 * 
 * This function (watchdog_pet) implements the safety-critical "Dead Man's Token" watchdog.
 * It ensures that if any critical system service hangs (e.g. motor controller SPI deadlock),
 * PID 1 will detect the missed check-in window and intentionally withhold the heartbeats 
 * to /dev/watchdog, causing the hardware watchdog chip to force-reset the system.
 * 
 * Crucially, if PID 1 itself hangs, deadlocks, or blocks indefinitely (e.g., due to a main
 * event loop lockup), no pets will be sent to /dev/watchdog. The hardware watchdog will
 * naturally timeout and force-reboot the processor. This guarantees a safe failure cascade
 * under all single-point failure modes.
 */
static void watchdog_init(void) {
    const char *paths[] = { "/dev/watchdog0", "/dev/watchdog", NULL };
    int i;
    for (i = 0; paths[i]; i++) {
        watchdog_fd = open(paths[i], O_WRONLY | O_CLOEXEC);
        if (watchdog_fd >= 0) {
            printf("[schema-init] opened hardware watchdog (%s)\n", paths[i]);
            break;
        }
    }
}

static void watchdog_close(void) {
    if (watchdog_fd >= 0) {
        /* Write 'V' before closing to indicate safe close/disable if supported */
        write(watchdog_fd, "V", 1);
        close(watchdog_fd);
        watchdog_fd = -1;
    }
}

static void watchdog_pet(void) {
    if (watchdog_fd < 0) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int i;
    int pet_ok = 1;
    for (i = 0; i < svc_count; i++) {
        service_t *svc = &services[i];
        if (svc->watchdog_timeout_ms > 0 && svc->child_pid > 0) {
            long elapsed_ms = (now.tv_sec - svc->last_pet.tv_sec) * 1000 +
                              (now.tv_nsec - svc->last_pet.tv_nsec) / 1000000;
            if (elapsed_ms > svc->watchdog_timeout_ms) {
                printf("[schema-init] watchdog: service '%s' missed pet window (%ldms > %dms)\n",
                       svc->name, elapsed_ms, svc->watchdog_timeout_ms);
                pet_ok = 0;
                break;
            }
        }
    }

    if (pet_ok) {
        write(watchdog_fd, "\0", 1);
    }
}

/* ── PID 1 essentials ───────────────────────────────────────────────── */

static void mount_pseudo(void) {
    /* only attempt mounts when we are PID 1 */
    if (getpid() != 1) return;

    /* kernel mounts rootfs ro for fsck; remount rw before anything else */
    mount(NULL, "/", NULL, MS_REMOUNT, NULL);

    mount("proc",    "/proc", "proc",     MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL);
    mount("sysfs",   "/sys",  "sysfs",    MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL);
    mount("devtmpfs","/dev",  "devtmpfs", MS_NOSUID|MS_STRICTATIME,     NULL);
    /* devtmpfs doesn't create these; userspace init must. Without them bash
     * process substitution and /dev/std{in,out,err} fail. */
    symlink("/proc/self/fd",   "/dev/fd");
    symlink("/proc/self/fd/0", "/dev/stdin");
    symlink("/proc/self/fd/1", "/dev/stdout");
    symlink("/proc/self/fd/2", "/dev/stderr");
    mkdir("/dev/pts", 0755);
    mount("devpts",  "/dev/pts", "devpts", MS_NOSUID|MS_NOEXEC,        "gid=5,mode=620,ptmxmode=666");
    mkdir("/dev/shm", 1777);
    mount("tmpfs",   "/dev/shm", "tmpfs", MS_NOSUID|MS_NODEV,          "mode=1777");
    mount("tmpfs",   "/run",  "tmpfs",    MS_NOSUID|MS_NODEV,           "mode=0755");
    mount("cgroup2", "/sys/fs/cgroup", "cgroup2", MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_RELATIME, NULL);
    int cg_fd = open("/sys/fs/cgroup/cgroup.subtree_control", O_WRONLY);
    if (cg_fd >= 0) {
        write(cg_fd, "+cpu +memory +cpuset +pids +io", 30);
        close(cg_fd);
    }
    mkdir("/run/dbus",     0755);
    mkdir("/run/lock",     1777);
    mkdir("/run/shm",      1777);
    mkdir("/run/user",              0755);
    mkdir("/run/user/0",            0700);
    mkdir("/run/systemd",           0755);
    mkdir("/run/systemd/system",    0755);   /* sd_booted() signal: libsystemd does access() here */
    mkdir("/run/systemd/shutdown",  0755);
    mkdir("/run/log",               0755);
    mkdir("/run/log/schema-init",   0755);
    mkdir("/run/sshd",              0755);
}

static void cleanup_tmp_locks(void) {
    glob_t globbuf;
    size_t i;
    /* Clean up stale Xorg lock files */
    if (glob("/tmp/.X*-lock", 0, NULL, &globbuf) == 0) {
        for (i = 0; i < globbuf.gl_pathc; i++) {
            unlink(globbuf.gl_pathv[i]);
        }
        globfree(&globbuf);
    }
    /* Clean up stale Xorg Unix socket files */
    if (glob("/tmp/.X11-unix/X*", 0, NULL, &globbuf) == 0) {
        for (i = 0; i < globbuf.gl_pathc; i++) {
            unlink(globbuf.gl_pathv[i]);
        }
        globfree(&globbuf);
    }
}

static void sig_term(int s)   { (void)s; running = 0; do_reboot = 0; if (shm_ptr) shm_ptr->system_state = 13; }
static void sig_int(int s)    { (void)s; running = 0; do_reboot = 1; if (shm_ptr) shm_ptr->system_state = 14; }
static void sig_usr1(int s)   { (void)s; running = 0; do_reboot = 0; }  /* halt */
static void sig_usr2(int s)   { (void)s; running = 0; do_reboot = 0; }  /* poweroff */

static void setup_signals(void) {
    struct sigaction sa;
    sigset_t chld;
    memset(&sa, 0, sizeof(sa));

    /* block SIGCHLD — signalfd will drain it in the epoll loop */
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld, NULL);

    sa.sa_handler = sig_term;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = sig_int;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);

    sa.sa_handler = sig_usr1;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    sa.sa_handler = sig_usr2;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGUSR2, &sa, NULL);

    /* PID 1 must not die on these */
    signal(SIGPIPE, SIG_IGN);
}

static void load_env_file(void) {
    FILE *f = fopen("/run/schema-init/env", "r");
    if (!f) {
        f = fopen("./run/schema-init/env", "r");
    }
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *start = line;
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

static void reap(void) {
    int status;
    pid_t pid;
    int i;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (i = 0; i < svc_count; i++) {
            if (services[i].child_pid != pid) continue;

            services[i].child_pid  = 0;
            services[i].exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

            if (services[i].flags & SVC_TIMER) {
                /* timer fire complete: re-arm regardless of exit code (cron
                 * semantics — a failed run isn't retried, it runs next window).
                 * interval 0 = run-once (only on_boot_sec): drop the flag so
                 * PERFECT becomes terminal instead of re-firing every tick. */
                services[i].inst.state = STATE_PERFECT;
                if (services[i].flags & SVC_TIMER_CALENDAR) {
                    if (services[i].flags & SVC_TIMER_PERSIST)
                        timer_stamp_write(&services[i], time(NULL));
                    timer_arm_calendar(&services[i]);
                } else if (services[i].timer_interval_sec > 0) {
                    struct timespec tn;
                    clock_gettime(CLOCK_MONOTONIC, &tn);
                    services[i].timer_next = tn;
                    services[i].timer_next.tv_sec += services[i].timer_interval_sec;
                } else {
                    services[i].flags &= ~SVC_TIMER;
                }
                service_log(&services[i],
                    services[i].exit_status == 0 ? "timer-done" : "timer-failed");
                break;
            }

            if (WIFEXITED(status) && WEXITSTATUS(status) == 0
                && (services[i].flags & SVC_ONESHOT)) {
                /* clean one-shot exit → PERFECT */
                services[i].inst.state = STATE_PERFECT;
                clock_gettime(CLOCK_MONOTONIC, &services[i].stable_time);
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
            } else {
                /* unexpected death → enter recovery arc */
                if (services[i].failsafe_cmd[0]) {
                    start_failsafe(&services[i]);
                }
                services[i].inst.state = STATE_RECOVERY;
                service_log(&services[i], "died");
            }
            break;
        }
    }
}

/* ── resource pressure and quarantine executive ─────────────────────── */

static double read_cpu_pressure(const char *cgroup_path) {
    char path[256];
    FILE *f;
    double avg10 = 0.0;
    if (!cgroup_path[0]) return 0.0;
    snprintf(path, sizeof(path), "%s/cpu.pressure", cgroup_path);
    f = fopen(path, "r");
    if (!f) return 0.0;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "avg10=");
        if (p) {
            sscanf(p + 6, "%lf", &avg10);
        }
    }
    fclose(f);
    return avg10;
}

static double read_system_mem_pressure(void) {
    FILE *f = fopen("/proc/pressure/memory", "r");
    double avg10 = 0.0;
    if (!f) return 0.0;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "avg10=");
        if (p) {
            sscanf(p + 6, "%lf", &avg10);
        }
    }
    fclose(f);
    return avg10;
}

static int check_system_pressure(void) {
    int i;
    for (i = 0; i < svc_count; i++) {
        service_t *svc = &services[i];
        if (svc->priority == PRIO_CRITICAL && svc->child_pid > 0) {
            double cpu_stalled = read_cpu_pressure(svc->cgroup_path);
            if (cpu_stalled > 5.0) {
                return 1;
            }
        }
    }
    double mem_stalled = read_system_mem_pressure();
    if (mem_stalled > 10.0) {
        return 1;
    }
    return 0;
}

static void set_cgroup_freeze(service_t *svc, int freeze) {
    char path[256];
    int fd;
    if (!svc->cgroup_path[0]) return;
    if (svc->is_frozen == freeze) return;
    snprintf(path, sizeof(path), "%s/cgroup.freeze", svc->cgroup_path);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, freeze ? "1\n" : "0\n", 2);
        close(fd);
        svc->is_frozen = freeze;
        service_log(svc, freeze ? "freeze" : "thaw");
    } else if (errno == ENOENT) {
        if (svc->child_pid > 0) {
            kill(svc->child_pid, freeze ? SIGSTOP : SIGCONT);
            svc->is_frozen = freeze;
            service_log(svc, freeze ? "freeze-sig" : "thaw-sig");
        }
    }
}

static void set_cgroup_cpu_limit(service_t *svc, const char *limit) {
    char path[256];
    int fd;
    if (!svc->cgroup_path[0]) return;
    snprintf(path, sizeof(path), "%s/cpu.max", svc->cgroup_path);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, limit, strlen(limit));
        close(fd);
    }
}

/* bytes of cold memory to ask the kernel to reclaim from svc's cgroup, or 0 to
 * skip. Critical services and tiny cgroups are left alone. Cheap (a single
 * memory.current read) — safe to run inline in PID 1. */
static long reclaim_read_target(const service_t *svc) {
    char path[256];
    long cur;
    FILE *f;
    if (!svc->cgroup_path[0]) return 0;
    if (svc->priority == PRIO_CRITICAL) return 0;
    snprintf(path, sizeof(path), "%s/memory.current", svc->cgroup_path);
    f = fopen(path, "r");
    if (!f) return 0;
    if (fscanf(f, "%ld", &cur) != 1) { fclose(f); return 0; }
    fclose(f);
    return reclaim_target(cur, RECLAIM_CAP);
}

/* The memory.reclaim write is SYNCHRONOUS — the kernel reclaims in the writer's
 * context. Run it only in the forked child (reclaim_pass), never in PID 1. */
static void reclaim_cgroup_write(const service_t *svc, long target) {
    char path[256], buf[32];
    int fd, n;
    snprintf(path, sizeof(path), "%s/memory.reclaim", svc->cgroup_path);
    fd = open(path, O_WRONLY);
    if (fd < 0) return;
    n = snprintf(buf, sizeof(buf), "%ld\n", target);
    if (n > 0) write(fd, buf, (size_t)n);
    close(fd);
}

/* Fork a helper to reclaim cold pages from non-critical cgroups. The parent
 * reads targets and logs (fast, non-blocking); the child performs the
 * synchronous memory.reclaim writes and exits, so PID 1 never stalls. */
static void reclaim_pass(void) {
    long targets[MAX_SERVICES];
    int i, any = 0;
    pid_t pid;
    for (i = 0; i < svc_count; i++) {
        targets[i] = 0;
        if (services[i].child_pid <= 0) continue;
        targets[i] = reclaim_read_target(&services[i]);
        if (targets[i] > 0) { service_log(&services[i], "reclaim"); any = 1; }
    }
    if (!any) return;
    pid = fork();
    if (pid != 0) return; /* parent, or fork failure: do not block PID 1 */
    service_reset_child_sigmask();
    for (i = 0; i < svc_count; i++)
        if (targets[i] > 0) reclaim_cgroup_write(&services[i], targets[i]);
    _exit(0);
}

static void execute_survival_posture(int under_pressure) {
    int i;
    for (i = 0; i < svc_count; i++) {
        service_t *svc = &services[i];
        if (svc->child_pid <= 0) continue;
        if (svc->priority == PRIO_PERIPHERAL) {
            set_cgroup_freeze(svc, under_pressure);
        } else if (svc->priority == PRIO_STANDARD) {
            set_cgroup_cpu_limit(svc, under_pressure ? "50000 100000" : "max 100000");
        }
    }
    /* On memory pressure, reclaim cold pages from non-critical cgroups. Peripheral
     * services are now frozen, so their pages are cold and reclaim cleanly. */
    if (under_pressure && read_system_mem_pressure() > 10.0)
        reclaim_pass();
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
    
    kill(svc->child_pid, SIGTERM);
    
    int elapsed_ms = 0;
    int status;
    while (elapsed_ms < 50) {
        pid_t res = waitpid(svc->child_pid, &status, WNOHANG);
        if (res == svc->child_pid) {
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
    while (elapsed_ms < 50) {
        if (waitpid(svc->child_pid, &status, WNOHANG) == svc->child_pid) break;
        usleep(1000);
        elapsed_ms++;
    }
    svc->child_pid = 0;
    service_cgroup_kill(svc);
}

/* ── schema tick for one service ────────────────────────────────────── */

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
            if (svc->failsafe_pid > 0) break;
            /* hold here silently until all deps reach a stable state */
            if (!service_deps_ready(svc, services, svc_count,
                                    grp_states, gcount)) break;
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
            if (svc->start_timeout_sec > 0 && svc->child_pid > 0 &&
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
            if (!(svc->flags & SVC_ONESHOT) && svc->child_pid > 0) {
                int ready = 0;
                if (svc->ready_path[0] && access(svc->ready_path, F_OK) == 0) {
                    ready = 1;
                    svc->ready_path_verified = 1;
                } else if (now_mono.tv_sec - svc->spawn_time_mono.tv_sec >= svc->stable_secs) {
                    ready = 1;
                }
                if (ready) {
                    flags = service_probe_f8(svc, services, svc_count);
                    schema_step(&svc->inst, flags);
                    if (svc->inst.state != prev) {
                        clock_gettime(CLOCK_MONOTONIC, &svc->stable_time);
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
                service_log(svc, "retry");
            } else if (svc->inst.state == STATE_FRICTION) {
                service_log(svc, "friction");
            }
            break;

        case STATE_FRICTION:
            flags = service_probe_f6(svc);
            schema_step(&svc->inst, flags);
            if (svc->inst.state == STATE_RECOVERY) {
                svc->inst.state = STATE_NEW_PROCESS;
                service_log(svc, "retry-deep");
            } else if (svc->inst.state == STATE_EXCISED) {
                service_cgroup_kill(svc);
                svc->dormant_count++;
                if (!(svc->flags & SVC_CRITICAL) && !svc->no_excise && svc->dormant_count > 4) {
                    service_log(svc, "76-excised");
                } else {
                    time_t delay = 300L << (svc->dormant_count - 1);
                    if (delay > 3600) delay = 3600;
                    struct timespec _now;
                    clock_gettime(CLOCK_MONOTONIC, &_now);
                    svc->dormant_until.tv_sec  = _now.tv_sec + delay;
                    svc->dormant_until.tv_nsec = _now.tv_nsec;
                    svc->inst.state = STATE_DORMANT;
                    service_log(svc, "dormant");
                }
            }
            break;

        case STATE_DORMANT: {
            struct timespec _now;
            clock_gettime(CLOCK_MONOTONIC, &_now);
            if (_now.tv_sec >= svc->dormant_until.tv_sec) {
                svc->inst.state = STATE_NEW_PROCESS;
                service_log(svc, "dormant-wake");
            }
            break;
        }

        case STATE_FUNDAMENTAL:
            if (svc->ready_path[0] && svc->child_pid > 0) {
                if (!svc->ready_path_verified) {
                    if (access(svc->ready_path, F_OK) == 0) {
                        svc->ready_path_verified = 1;
                    }
                }
                if (svc->ready_path_verified) {
                    int ticks_to_wait = 1;
                    if (svc->ready_poll_hz > 0) {
                        int loop_hz = 1000000 / TICK_USEC;
                        if (loop_hz > svc->ready_poll_hz) {
                            ticks_to_wait = loop_hz / svc->ready_poll_hz;
                            if (ticks_to_wait < 1) ticks_to_wait = 1;
                        }
                    }
                    svc->ready_check_ticks++;
                    if (svc->ready_check_ticks >= ticks_to_wait) {
                        svc->ready_check_ticks = 0;
                        if (access(svc->ready_path, F_OK) != 0) {
                            service_log(svc, "readiness-lost");
                            active_kill_service(svc);
                            start_failsafe(svc);
                            
                            svc->dormant_count++;
                            if (!(svc->flags & SVC_CRITICAL) && !svc->no_excise && svc->dormant_count > 4) {
                                svc->inst.state = STATE_EXCISED;
                                service_log(svc, "76-excised");
                            } else {
                                time_t delay = 300L << (svc->dormant_count - 1);
                                if (delay > 3600) delay = 3600;
                                struct timespec _now2;
                                clock_gettime(CLOCK_MONOTONIC, &_now2);
                                svc->dormant_until.tv_sec  = _now2.tv_sec + delay;
                                svc->dormant_until.tv_nsec = _now2.tv_nsec;
                                svc->inst.state = STATE_DORMANT;
                                service_log(svc, "dormant");
                            }
                        }
                    }
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
    int fd = shm_open(SCHEMA_SHM_NAME, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return;
    ftruncate(fd, (off_t)sizeof(schema_shm_t));
    shm_ptr = mmap(NULL, sizeof(schema_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm_ptr == MAP_FAILED) shm_ptr = NULL;
}

static void shm_update(void) {
    int i;
    if (!shm_ptr) return;
    for (i = 0; i < svc_count; i++) {
        memcpy(shm_ptr->svc[i].name, services[i].name, 64);
        shm_ptr->svc[i].state         = services[i].inst.state;
        shm_ptr->svc[i].weight        = services[i].inst.weight;
        shm_ptr->svc[i].child_pid     = (int32_t)services[i].child_pid;
        shm_ptr->svc[i].restart_count = services[i].restart_count;
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

static void ctl_writef(int fd, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) write(fd, buf, (size_t)n);
}

static void ctl_cmd(int fd, char *line) {
    int i;
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
            }
        } else {
            ctl_writef(fd, "services: %d  groups: %d\n", svc_count, grp_count);
            for (i = 0; i < svc_count; i++)
                ctl_writef(fd, "  %-24s  pid=%-6d  state=%-14s  restarts=%d\n",
                    services[i].name, (int)services[i].child_pid,
                    state_name(services[i].inst.state), services[i].restart_count);
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
                services[i].inst.state   = STATE_NEW_PROCESS;
                services[i].restart_count = 0;
                ctl_writef(fd, "ok: %s queued\n", name);
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
            if (services[i].child_pid > 0) {
                kill(services[i].child_pid, SIGTERM);
                ctl_writef(fd, "ok: SIGTERM → %s (pid %d)\n",
                    name, (int)services[i].child_pid);
            } else {
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
            if (services[i].child_pid > 0) {
                kill(services[i].child_pid, SIGTERM);
                ctl_writef(fd, "ok: SIGTERM → %s — recovery arc will respawn\n", name);
            } else {
                services[i].inst.state    = STATE_NEW_PROCESS;
                services[i].restart_count = 0;
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
                write(fd, ".\n", 2);
                return;
            }
        }
        if (validate_and_resolve(services, svc_count + 1, groups, grp_count) > 0) {
            ctl_writef(fd, "err: '%s' introduces a dependency cycle — rejected\n", services[svc_count].name);
            for (k = 1; k < MAX_ARGV; k++)
                if (services[svc_count].argv[k]) { free(services[svc_count].argv[k]); services[svc_count].argv[k] = NULL; }
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
        double k2p = (double)init_start.tv_sec + (double)init_start.tv_nsec / 1e9;
        ctl_writef(fd, "kernel → PID 1:            %.3fs\n", k2p);
        for (i = 0; i < svc_count; i++) {
            double delta;
            if (services[i].stable_time.tv_sec == 0) continue;
            delta = (double)(services[i].stable_time.tv_sec - init_start.tv_sec)
                  + (double)(services[i].stable_time.tv_nsec - init_start.tv_nsec) / 1e9;
            ctl_writef(fd, "  %-24s %.3fs\n", services[i].name, delta);
        }

    } else if (strcmp(line, "reboot") == 0) {
        ctl_writef(fd, "ok: rebooting\n");
        if (shm_ptr) shm_ptr->system_state = 14;
        do_reboot = 1;
        running = 0;

    } else if (strcmp(line, "poweroff") == 0) {
        ctl_writef(fd, "ok: powering off\n");
        if (shm_ptr) shm_ptr->system_state = 13;
        do_reboot = 0;
        running = 0;

    } else {
        ctl_writef(fd, "err: unknown: %s\n", line);
    }
    write(fd, ".\n", 2);
}

static void set_nonblock(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void ctl_init(void) {
    struct sockaddr_un addr;
    unlink(CTL_SOCK_PATH);
    ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (ctl_fd < 0) return;
    set_nonblock(ctl_fd);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CTL_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        /* try local fallback path */
        mkdir("./run", 0755);
        unlink("./run/schema-init.sock");
        strncpy(addr.sun_path, "./run/schema-init.sock", sizeof(addr.sun_path) - 1);
        if (bind(ctl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(ctl_fd); ctl_fd = -1; return;
        }
    }
    /* If a "schema" group exists, grant it read-only access (status/list/timing)
     * by opening the socket to root:schema 0660. Writes are still gated per-call
     * by peer uid in ctl_poll(). No group → root-only 0600 as before. */
    {
        struct group *g = getgrnam("schema");
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
        || (n == 6 && memcmp(line, "timing", 6) == 0);
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

    /* Peer identity: root may run everything; schema-group members (allowed in
     * by the 0660 socket) get read-only verbs only. */
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
        if (peer_uid != 0 && !ctl_is_readonly(buf)) {
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
    sigprocmask(SIG_BLOCK, &mask, NULL);
    sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd >= 0) {
        set_nonblock(sig_fd);
    }
}

/* ── boot sequence ──────────────────────────────────────────────────── */

static void schema_boot_log(void) {
    printf("[schema-init] boot rail\n");
    printf("  [1] kernel core alive\n");
    printf("  [2] init\n");
    printf("  [3] runlevel\n");
    printf("  [4] env load\n");
    printf("  [5] env active\n");
    printf("  [7] settled — %d service(s) queued\n", svc_count);
    fflush(stdout);
}

static int get_poll_timeout(void) {
    if (system_under_pressure) {
        return TICK_USEC / 1000;
    }
    int i;
    int has_watchdog_services = 0;
    for (i = 0; i < svc_count; i++) {
        uint8_t s = services[i].inst.state;
        if (s != STATE_FUNDAMENTAL && s != STATE_PERFECT && s != STATE_EXCISED) {
            return TICK_USEC / 1000;
        }
        if (s == STATE_FUNDAMENTAL && services[i].ready_path[0] && services[i].child_pid > 0) {
            return TICK_USEC / 1000;
        }
        if (services[i].watchdog_timeout_ms > 0 && services[i].child_pid > 0) {
            has_watchdog_services = 1;
        }
    }
    if (has_watchdog_services) {
        return TICK_USEC / 1000;
    }
    if (watchdog_fd >= 0) {
        /* Hardware watchdog is active; wake up at least every 5 seconds to pet it */
        return 5000;
    }
    return -1;
}

static int validate_and_resolve(service_t *svc_table, int s_count, group_t *grp_table, int g_count) {
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
                        printf("[schema-init] WARNING: service '%s' depends on timer service '%s'. Dependents of timer services may be transiently blocked when the timer fires.\n",
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
    printf("[schema-init] reloading configuration (SIGHUP)... \n");

    const char *svc_dir = SVC_DIR;
    memset(shadow_services, 0, sizeof(service_t) * MAX_SERVICES);
    int shadow_count = services_load(svc_dir, shadow_services, MAX_SERVICES);
    if (shadow_count == 0) {
        shadow_count = services_load("./services", shadow_services, MAX_SERVICES);
    }
    if (shadow_count <= 0) {
        printf("[schema-init] reload failed: no services loaded from configuration\n");
        if (err) snprintf(err, errsz, "no services loaded from configuration");
        return -1;
    }

    memset(shadow_groups, 0, sizeof(group_t) * MAX_GROUPS);
    int shadow_grp_count = groups_load(svc_dir, shadow_groups, MAX_GROUPS);
    if (shadow_grp_count == 0) {
        shadow_grp_count = groups_load("./services", shadow_groups, MAX_GROUPS);
    }

    /* hash integrity check — reject any .svc file modified since boot */
    for (i = 0; i < shadow_count; i++) {
        for (j = 0; j < svc_count; j++) {
            if (strcmp(shadow_services[i].name, services[j].name) != 0) continue;
            if (shadow_services[i].content_hash != services[j].content_hash &&
                shadow_services[i].content_hash != 0) {
                printf("[schema-init] INTEGRITY: '%s' hash mismatch — file modified since boot; reload rejected\n",
                       shadow_services[i].name);
                if (err) snprintf(err, errsz,
                    "'%s' modified since boot — integrity check rejected the reload; "
                    "a changed .svc takes effect at next boot",
                    shadow_services[i].name);
                for (int k = 0; k < shadow_count; k++)
                    for (int m = 1; m < MAX_ARGV; m++)
                        if (shadow_services[k].argv[m]) { free(shadow_services[k].argv[m]); shadow_services[k].argv[m] = NULL; }
                return -1;
            }
            break;
        }
    }

    if (validate_and_resolve(shadow_services, shadow_count, shadow_groups, shadow_grp_count) > 0) {
        printf("[schema-init] reload rejected: dependency cycles detected in new configuration\n");
        if (err) snprintf(err, errsz, "dependency cycle in new configuration");
        /* free duplicated arguments in shadow array to prevent leaks */
        for (i = 0; i < shadow_count; i++) {
            for (j = 1; j < MAX_ARGV; j++) {
                if (shadow_services[i].argv[j]) {
                    free(shadow_services[i].argv[j]);
                }
            }
        }
        return -1;
    }

    /* Merge running state from live to shadow */
    for (i = 0; i < shadow_count; i++) {
        for (j = 0; j < svc_count; j++) {
            if (strcmp(shadow_services[i].name, services[j].name) == 0) {
                shadow_services[i].child_pid     = services[j].child_pid;
                shadow_services[i].inst          = services[j].inst;
                shadow_services[i].restart_count = services[j].restart_count;
                shadow_services[i].dormant_count = services[j].dormant_count;
                shadow_services[i].dormant_until = services[j].dormant_until;
                shadow_services[i].last_start    = services[j].last_start;
                shadow_services[i].start_time    = services[j].start_time;
                shadow_services[i].stable_time   = services[j].stable_time;
                shadow_services[i].failsafe_pid  = services[j].failsafe_pid;
                shadow_services[i].failsafe_start = services[j].failsafe_start;
                shadow_services[i].last_pet      = services[j].last_pet;
                shadow_services[i].ready_path_verified = services[j].ready_path_verified;
                shadow_services[i].timer_next    = services[j].timer_next;
                shadow_services[i].spawn_time_mono = services[j].spawn_time_mono;

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
                if ((shadow_services[i].flags & SVC_TIMER) &&
                    !(services[j].flags & SVC_TIMER) &&
                    !(shadow_services[i].flags & SVC_TIMER_CALENDAR) &&
                    shadow_services[i].timer_interval_sec <= 0) {
                    shadow_services[i].flags &= ~SVC_TIMER;
                }

                /* clear live service state so we don't accidentally treat it as running/managed */
                services[j].child_pid = 0;
                services[j].failsafe_pid = 0;
                break;
            }
        }
    }

    /* Identify and clean up orphaned running services */
    for (j = 0; j < svc_count; j++) {
        if (services[j].child_pid > 0 || services[j].failsafe_pid > 0) {
            printf("[schema-init] service '%s' is orphaned in new configuration\n", services[j].name);
            if (evict_mode) {
                printf("[schema-init] evicting orphaned service '%s' (pid=%d)\n", services[j].name, (int)services[j].child_pid);
                if (services[j].child_pid > 0) {
                    kill(services[j].child_pid, SIGTERM);
                    if (eviction_count < MAX_EVICTIONS) {
                        evictions[eviction_count].pid      = services[j].child_pid;
                        evictions[eviction_count].deadline = time(NULL) + EVICT_GRACE_SECS;
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
                printf("[schema-init] service '%s' (pid=%d) running unmanaged to natural death\n", services[j].name, (int)services[j].child_pid);
            }
        }
        /* Free duplicated arguments from the old configuration array */
        for (int k = 1; k < MAX_ARGV; k++) {
            if (services[j].argv[k]) {
                free(services[j].argv[k]);
                services[j].argv[k] = NULL;
            }
        }
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

    printf("[schema-init] configuration reload completed successfully (generation updated)\n");

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

/* ── main ───────────────────────────────────────────────────────────── */

static void usage(FILE *out) {
    fprintf(out,
        "Usage: schema-init [SERVICE_DIR]\n"
        "\n"
        "schema-init is an init system. The kernel starts it as PID 1; it is not\n"
        "a command-line tool and refuses to run as any other pid.\n"
        "\n"
        "  SERVICE_DIR    service definitions (default: %s)\n"
        "  -h, --help     show this help and exit\n"
        "  -V, --version  show version and exit\n"
        "\n"
        "To inspect or control a running schema-init, use schema-ctl:\n"
        "  schema-ctl status | timing | list | reload | start NAME | stop NAME\n",
        SVC_DIR);
}

/* PID 1 must never be able to block on a tty. /dev/console follows the
 * FOREGROUND VT, and a session VT left in KD_TEXT with a live line discipline
 * turns one stray ^S into tty->flow.stopped: do_con_write() accepts 0 bytes
 * from then on and every write sleeps in n_tty_write() under
 * MAX_SCHEDULE_TIMEOUT. No timeout, no signal, no way out. On 2026-07-26 that
 * caught PID 1 on the FIRST printf of this path and hung the box -- services
 * never signalled, nothing synced, no reboot. Own console fd, O_NONBLOCK,
 * TCOON to clear any stop we inherited. A dropped log line beats a dead init. */
static int shut_fd = -1;

/* The console is the only place shutdown has ever reported to, and it scrolls.
 * journal-sink is dead by then, so the fifteen seconds that matter most are the
 * one stretch with no recoverable log -- on 2026-07-26 the verdict had to be
 * read off a phone photo. Buffer the transcript in memory and put it on disk in
 * one go, deliberately NOT as we write it: touching the filesystem early would
 * hand PID 1 a second way to block. shut_flush_log() runs after bounded_sync()
 * and before the mount holding it goes read-only. */
#define SHUT_LOG_PATH "/var/log/schema-init/shutdown.log"
#define SHUT_LOG_MAX  4096
static char shut_buf[SHUT_LOG_MAX];
static size_t shut_len = 0;
static size_t shut_flushed = 0;

static void shut_console_open(void) {
    time_t now = time(NULL);
    struct tm tm;

    shut_fd = open("/dev/console", O_WRONLY | O_NONBLOCK | O_NOCTTY);
    if (shut_fd >= 0) tcflow(shut_fd, TCOON);

    if (localtime_r(&now, &tm))
        shut_len = strftime(shut_buf, sizeof(shut_buf),
                            "=== shutdown %Y-%m-%d %H:%M:%S ===\n", &tm);
}

static void shut_write(const char *s) {
    size_t n = strlen(s);

    if (shut_fd >= 0) {
        ssize_t w = write(shut_fd, s, n);
        (void)w;                       /* short or EAGAIN: drop it, never wait */
    } else {
        printf("%s", s);
        fflush(stdout);
    }

    if (shut_len + n < sizeof(shut_buf)) {
        memcpy(shut_buf + shut_len, s, n);
        shut_len += n;
    }
}

/* Best effort by design: if this fails the reboot still has to happen. Appends
 * only what has not been written yet, so it is safe to call more than once --
 * the call before the remount sweep is the one that always lands, the call
 * after it adds the sweep's own verdict if the filesystem is still writable. */
static void shut_flush_log(void) {
    int fd;

    if (shut_len <= shut_flushed) return;
    mkdir("/var/log/schema-init", 0755);
    fd = open(SHUT_LOG_PATH, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) return;
    if (write(fd, shut_buf + shut_flushed, shut_len - shut_flushed) > 0) {
        fsync(fd);
        shut_flushed = shut_len;
    }
    close(fd);
}

static void shut_log(const char *msg) {
    char buf[384];                 /* >= the 320-byte mount-path messages */
    snprintf(buf, sizeof(buf), "[schema-init] shutdown: %s\n", msg);
    shut_write(buf);
}

/* A FUSE mount whose daemon we just cgroup-killed can leave the connection
 * live with nobody to answer it; anything that then touches the superblock
 * (sync, umount, remount) waits in D state forever. Abort them first. */
static void fuse_abort_all(void) {
    glob_t g;
    size_t n;

    if (glob("/sys/fs/fuse/connections/*/abort", 0, NULL, &g) != 0) return;
    for (n = 0; n < g.gl_pathc; n++) {
        int fd = open(g.gl_pathv[n], O_WRONLY);
        if (fd >= 0) { write(fd, "1", 1); close(fd); }
    }
    if (g.gl_pathc) shut_log("fuse connections aborted");
    globfree(&g);
}

/* Containers escape the .svc cgroup sweep. docker's cgroupfs driver puts each
 * container under /sys/fs/cgroup/docker/<id> -- a top-level cgroup root that no
 * .svc owns, so service_cgroup_kill() (which only walks a service's own cgroup
 * under /sys/fs/cgroup/schema-init/<name>) never reaches it. On 2026-07-26
 * Frigate held frigate.db open there and the remount-ro below returned EBUSY,
 * leaving a dirty ext4 journal for the next boot to replay. kill(-1) does clear
 * them, but only by SIGKILL -- no chance to flush. Signalling the container
 * processes here, in the same grace window the services get, lets a container
 * shut down cleanly first; the cgroup.kill pass is the recursive backstop. */
static const char *CONTAINER_CGROUP_PROC_GLOBS[] = {
    "/sys/fs/cgroup/docker/*/cgroup.procs",
    "/sys/fs/cgroup/docker/*/*/cgroup.procs",
    "/sys/fs/cgroup/libpod_parent/*/cgroup.procs",
    "/sys/fs/cgroup/libpod_parent/*/*/cgroup.procs",
    NULL
};

static void container_cgroups_term(void) {
    glob_t g;
    size_t n;
    int i, have = 0;

    memset(&g, 0, sizeof g);
    for (i = 0; CONTAINER_CGROUP_PROC_GLOBS[i]; i++) {
        if (glob(CONTAINER_CGROUP_PROC_GLOBS[i],
                 have ? GLOB_APPEND : 0, NULL, &g) == 0)
            have = 1;
    }
    if (!have) return;

    for (n = 0; n < g.gl_pathc; n++) {
        FILE *f = fopen(g.gl_pathv[n], "r");
        pid_t p;
        if (!f) continue;
        while (fscanf(f, "%d", &p) == 1)
            if (p > 1) kill(p, SIGTERM);
        fclose(f);
    }
    if (g.gl_pathc) shut_log("container SIGTERM sent");
    globfree(&g);
}

static void container_cgroups_kill(void) {
    static const char *kill_globs[] = {
        "/sys/fs/cgroup/docker/*/cgroup.kill",
        "/sys/fs/cgroup/libpod_parent/*/cgroup.kill",
        NULL
    };
    glob_t g;
    size_t n;
    int i, have = 0;

    memset(&g, 0, sizeof g);
    for (i = 0; kill_globs[i]; i++) {
        if (glob(kill_globs[i], have ? GLOB_APPEND : 0, NULL, &g) == 0)
            have = 1;
    }
    if (!have) return;

    for (n = 0; n < g.gl_pathc; n++) {
        int fd = open(g.gl_pathv[n], O_WRONLY);
        if (fd >= 0) { write(fd, "1", 1); close(fd); }
    }
    if (g.gl_pathc) shut_log("container cgroups killed");
    globfree(&g);
}

/* sync() is unbounded: one wedged filesystem and PID 1 never reaches
 * reboot(). Run it in a child so the deadline is ours, not the kernel's. */
static void bounded_sync(int secs) {
    pid_t p = fork();
    int slice;

    if (p < 0) { shut_log("sync fork failed - skipping"); return; }
    if (p == 0) { sync(); _exit(0); }

    for (slice = 0; slice < secs * 10; slice++) {
        if (waitpid(p, NULL, WNOHANG) == p) { shut_log("sync done"); return; }
        usleep(100000);
    }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
    shut_log("sync timed out - continuing");
}

/* An overlay mount's option string runs well past any sane line buffer, and a
 * bare fgets() then hands back the tail of that line as if it were the next
 * mount -- sscanf turns the fragment into a mountpoint that does not exist,
 * and 128 of those crowd the real filesystems out of the sweep. The first
 * three fields always fit, so parse the head and drop the rest of the line. */
static char *read_mount_line(FILE *f, char *line, size_t len) {
    if (!fgets(line, len, f)) return NULL;
    if (!strchr(line, '\n')) {
        int c;
        while ((c = fgetc(f)) != '\n' && c != EOF)
            ;
    }
    return line;
}

/* Which device backs "/". Read on its own pass because /proc/mounts is not
 * required to list "/" first, and the sweep below needs the answer up front. */
static void root_source(char *out, size_t len) {
    char line[512];
    FILE *f;

    *out = '\0';
    f = fopen("/proc/mounts", "r");
    if (!f) return;
    while (read_mount_line(f, line, sizeof(line))) {
        char dev[256], mp[256];
        if (sscanf(line, "%255s %255s", dev, mp) != 2) continue;
        if (!strcmp(mp, "/")) { snprintf(out, len, "%s", dev); break; }
    }
    fclose(f);
}

/* Nothing else in the shutdown path takes a filesystem read-only, so every
 * reboot leaves the journals dirty and the next boot replays them. Walk
 * /proc/mounts backwards (children before parents) and remount ro. "/" is
 * held out of that walk and done last on its own, so the shutdown transcript
 * can be flushed while the filesystem holding it is still writable. */
static void remount_all_ro(void) {
    static const char *skip[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "cgroup2", "ramfs",
        "securityfs", "pstore", "bpf", "autofs", "hugetlbfs", "mqueue",
        "debugfs", "tracefs", "configfs", "fusectl", "efivarfs",
        "binfmt_misc", "nsfs", "rpc_pipefs", "selinuxfs",
        "overlay", "squashfs", "iso9660", "rootfs", NULL
    };
    char mounts[MAX_MOUNTS][256];
    char rootdev[256];
    int count = 0, k, n;
    char line[512];
    FILE *f;

    root_source(rootdev, sizeof(rootdev));
    f = fopen("/proc/mounts", "r");
    if (!f) { shut_log("remount-ro: no /proc/mounts"); return; }
    while (count < MAX_MOUNTS && read_mount_line(f, line, sizeof(line))) {
        char dev[256], mp[256], type[64];
        if (sscanf(line, "%255s %255s %63s", dev, mp, type) != 3) continue;
        for (k = 0; skip[k]; k++)
            if (!strcmp(type, skip[k])) break;
        if (skip[k]) continue;
        if (!strncmp(type, "fuse", 4)) continue;   /* daemon is already gone */
        if (!strcmp(mp, "/")) continue;            /* held back, see below */

        /* MS_REMOUNT without MS_BIND reconfigures the SUPERBLOCK, not the
         * mount. Every btrfs subvolume and every bind mount of / reports the
         * same source device and shares that superblock, so remounting one of
         * them ro takes / ro with it -- on 2026-07-26 /var/lib/docker did
         * exactly that, and shut_flush_log() below then hit EROFS and the
         * shutdown transcript was never written. Holding back the string "/"
         * does not hold back the filesystem. Skip its siblings; the single
         * "/" remount at the end covers all of them at once. */
        if (*rootdev && !strcmp(dev, rootdev)) continue;

        snprintf(mounts[count], sizeof(mounts[0]), "%s", mp);
        count++;
    }
    fclose(f);

    for (n = count - 1; n >= 0; n--) {
        char msg[320];

        if (mount(NULL, mounts[n], NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0)
            continue;

        snprintf(msg, sizeof(msg), "remount-ro %s failed: %s",
                 mounts[n], strerror(errno));
        shut_log(msg);

        /* Last resort, and a weak one: a lazy umount detaches the tree but
         * does not put the superblock while anyone still holds a file open,
         * so a journal dirtied by a live writer stays dirty. It only helps
         * when the mount is busy as a mountpoint rather than as a file. The
         * real answer to EBUSY here is the SIGKILL broadcast upstream. */
        if (umount2(mounts[n], MNT_DETACH) == 0) {
            snprintf(msg, sizeof(msg), "  %s lazily detached", mounts[n]);
            shut_log(msg);
        }
    }
    shut_log("filesystems read-only");

    /* Everything above is on disk before the mount holding the log goes ro. */
    shut_flush_log();
    if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) != 0) {
        char msg[320];
        snprintf(msg, sizeof(msg), "remount-ro / failed: %s", strerror(errno));
        shut_write(msg);                             /* console only: / is ro */
        shut_write("\n");
    }
}

int main(int argc, char **argv) {
    int i;
    const char *svc_dir = SVC_DIR;
    int have_dir = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) {
            printf("schema-init %s\n", SCHEMA_INIT_VERSION);
            return 0;
        }
        /* As PID 1 the kernel appends every boot-cmdline word it didn't
         * consume (rhgb, splash, quiet, plymouth.debug, ...) to our argv.
         * Those tokens are not ours: letting a stray one become svc_dir makes
         * services_load() open a bogus directory and silently start zero
         * services. Only a hand-run invocation may name a services dir; as
         * PID 1, ignore all positional arguments and keep the built-in SVC_DIR. */
        if (getpid() == 1)
            continue;
        if (argv[i][0] == '-') {
            fprintf(stderr, "schema-init: unknown option '%s'\n", argv[i]);
            usage(stderr);
            return 1;
        }
        if (have_dir) {
            fprintf(stderr, "schema-init: too many arguments\n");
            usage(stderr);
            return 1;
        }
        svc_dir = argv[i];
        have_dir = 1;
    }

    if (getpid() != 1) {
        fprintf(stderr,
            "schema-init: refusing to run as pid %d — schema-init must be PID 1.\n"
            "Running it by hand would take the control socket (%s) away from the\n"
            "real PID 1 and start a second copy of every service.\n"
            "\n"
            "Did you mean:  schema-ctl status\n",
            (int)getpid(), CTL_SOCK_PATH);
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &init_start);

    /* Detach from controlling terminal to prevent receiving SIGINT on Ctrl+C */
    {
        int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (fd >= 0) {
            ioctl(fd, TIOCNOTTY);
            close(fd);
        }
    }
    mount_pseudo();
    cleanup_tmp_locks();
    watchdog_init();
    /* Before the control socket opens or a single service spawns, so the
     * raised limit covers every descriptor PID 1 will ever hold. */
    service_raise_pid1_nofile();
    setup_signals();

    svc_count = services_load(svc_dir, services, MAX_SERVICES);
    if (svc_count == 0) {
        svc_count = services_load("./services", services, MAX_SERVICES);
    }

    grp_count = groups_load(svc_dir, groups, MAX_GROUPS);
    if (grp_count == 0) {
        grp_count = groups_load("./services", groups, MAX_GROUPS);
    }

    if (validate_and_resolve(services, svc_count, groups, grp_count) > 0) {
        fprintf(stderr, "schema-init: dependency cycles detected — dropping to rescue shell\n");
        pid_t rsh = fork();
        if (rsh == 0) {
            service_reset_child_sigmask();
            setsid();
            int tty = open("/dev/tty1", O_RDWR);
            if (tty >= 0) {
                dup2(tty, STDIN_FILENO);
                dup2(tty, STDOUT_FILENO);
                dup2(tty, STDERR_FILENO);
                write(tty, "\n\n*** schema-init: DEPENDENCY CYCLE DETECTED ***\n", 50);
                write(tty, "Dropping to emergency rescue shell. Type 'exit' to halt.\n\n", 58);
                close(tty);
            }
            execl("/bin/sh", "sh", NULL);
            _exit(1);
        }
        if (rsh > 0)
            waitpid(rsh, NULL, 0);
        sync();
        reboot(RB_HALT_SYSTEM);
        return 1;
    }

    shm_init();
    ctl_init();
    signalfd_init();
    schema_boot_log();

    /* arm timer services: born "already ran", first fire = boot + on_boot_sec */
    {
        struct timespec tnow;
        clock_gettime(CLOCK_MONOTONIC, &tnow);
        for (int ti = 0; ti < svc_count; ti++) {
            if (services[ti].flags & SVC_TIMER) {
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

    while (running) {
        struct pollfd fds[2];
        int nfds = 0;
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

        if (nfds > 0) {
            ret = poll(fds, nfds, get_poll_timeout());
            if (ret > 0) {
                int e;
                for (e = 0; e < nfds; e++) {
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
                            }
                        }
                    } else if (fds[e].fd == ctl_fd) {
                        ctl_poll();
                    }
                }
            } else if (ret < 0 && errno != EINTR) {
                usleep(TICK_USEC);
            }
        } else {
            usleep(TICK_USEC);
        }

        /* Check resource pressure and toggle survival posture with hysteresis */
        int pressure = check_system_pressure();
        if (pressure) {
            pressure_clear_ticks = 0;
            if (!system_under_pressure) {
                system_under_pressure = 1;
                execute_survival_posture(1);
            }
        } else {
            if (system_under_pressure) {
                pressure_clear_ticks++;
                if (pressure_clear_ticks >= 8) { /* ~2 seconds of clean ticks */
                    system_under_pressure = 0;
                    pressure_clear_ticks = 0;
                    execute_survival_posture(0);
                }
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
        for (i = 0; i < svc_count; i++)
            tick_service(&services[i], grp_states, grp_count);
        monitor_failsafes();
        groups_update(groups, grp_count, svc_states, svc_count);
        shm_update();
        eviction_tick();
        watchdog_pet();
    }

    /* shutdown: hold briefly so viewers see system_state 13/14, then kill */
    watchdog_close();
    usleep(500000);
    shut_console_open();
    shut_write("[schema-init] shutting down\n");
    for (i = 0; i < svc_count; i++) {
        if (services[i].child_pid > 0)
            kill(services[i].child_pid, SIGTERM);
    }
    /* Containers do not hang off a .svc, so SIGTERM them here in the same
     * grace window -- a clean stop lets Frigate flush frigate.db before the
     * remount-ro below, instead of relying on the kill(-1) SIGKILL further on. */
    container_cgroups_term();
    shut_log("SIGTERM sent");
    sleep(3);
    for (i = 0; i < svc_count; i++) {
        service_cgroup_kill(&services[i]);
    }
    /* Recursively SIGKILL any container subtree that ignored the SIGTERM,
     * before the kill(-1) backstop, so their files are released for remount. */
    container_cgroups_kill();
    shut_log("cgroups killed");

    /* The cgroup sweep only reaches what a .svc owns. Containers live under
     * the runtime's own cgroup, not docker.svc's (handled just above now), so
     * anything still left -- on 2026-07-26 Frigate kept frigate.db open on
     * /mnt/extdrive and the remount-ro below returned EBUSY, leaving an ext4
     * journal the next boot had to replay. Kill whatever remains. Linux exempts
     * PID 1 from kill(-1), so this cannot hit us. */
    kill(-1, SIGKILL);
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    shut_log("SIGKILL broadcast");

    if (ctl_fd >= 0) {
        close(ctl_fd);
        unlink(CTL_SOCK_PATH);
        unlink("./run/schema-init.sock");
    }

    if (shm_ptr) {
        munmap(shm_ptr, sizeof(schema_shm_t));
        shm_unlink(SCHEMA_SHM_NAME);
    }
    shut_log("control socket and shm released");

    if (getpid() == 1) {
        fuse_abort_all();
        bounded_sync(10);
        shut_flush_log();        /* the sweep is the last thing that can lose it */
        remount_all_ro();
        if (do_reboot) {
            shut_write("[schema-init] PID 1 reboot\n");
            reboot(RB_AUTOBOOT);
        } else {
            shut_write("[schema-init] PID 1 poweroff\n");
            reboot(RB_POWER_OFF);
        }
    }

    /* free duplicated arguments to prevent memory leaks */
    for (i = 0; i < svc_count; i++) {
        int j;
        for (j = 1; j < MAX_ARGV; j++) {
            if (services[i].argv[j]) {
                free(services[i].argv[j]);
                services[i].argv[j] = NULL;
            }
        }
    }

    return 0;
}
