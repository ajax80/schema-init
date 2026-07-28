#include "service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <sys/resource.h>

/* PID 1 runs its main loop with SIGCHLD and SIGHUP blocked -- it reaps through
 * signalfd, so that is correct for us and wrong for everyone else. exec resets
 * handlers to SIG_DFL but PRESERVES the mask, so every child we spawn inherits
 * a blocked SIGCHLD unless it clears the mask itself. A child that reaps its own
 * children then never sees them exit: crond collected 55 zombies this way on
 * 2026-07-12. Go binaries escape because their runtime resets the mask at
 * startup; C daemons do not. Clear it in the child, between fork and exec. */
void service_reset_child_sigmask(void) {
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
}

/* ── F8: can we spawn this right now? ──────────────────────────────── */

static long free_mem_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long kb = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line + 13, "%ld", &kb);
            break;
        }
    }
    fclose(f);
    return kb;
}

uint32_t service_probe_f8(service_t *svc, service_t *table, int count) {
    uint32_t f = 0;
    int i, dep_ok = 1, dep_stable = 1;
    long mem;

    /* F8_HW_EXISTS / F8_HW_RESPONDS — binary on disk and executable */
    if (access(svc->exec, F_OK) == 0) f |= F8_HW_EXISTS;
    if (access(svc->exec, X_OK) == 0) f |= F8_HW_RESPONDS;

    /* F8_DEP_PRESENT / F8_DEP_STATE — all dependencies exist and are stable */
    for (i = 0; i < MAX_DEPS && svc->dep_idx[i] >= 0; i++) {
        int di = svc->dep_idx[i];
        if (di >= count) { dep_ok = 0; break; }
        if (table[di].inst.state == STATE_EXCISED) { dep_ok = 0; break; }
        if (table[di].inst.state != STATE_FUNDAMENTAL &&
            table[di].inst.state != STATE_SETTLED     &&
            table[di].inst.state != STATE_PERFECT)
            dep_stable = 0;
    }
    if (dep_ok)     f |= F8_DEP_PRESENT;
    if (dep_stable) f |= F8_DEP_STATE;

    /* F8_MEM_AVAIL / F8_MEM_SAFE */
    mem = free_mem_kb();
    if (mem > 0)             f |= F8_MEM_AVAIL;
    if (mem >= MEM_MIN_KB)   f |= F8_MEM_SAFE;

    /* F8_PERM_PRESENT / F8_PERM_AUTH */
    f |= F8_PERM_PRESENT;
    if (!(svc->flags & SVC_NEEDS_ROOT) || geteuid() == 0)
        f |= F8_PERM_AUTH;

    return f;
}

/* ── F9: can we recover after death? ───────────────────────────────── */

uint32_t service_probe_f9(service_t *svc, service_t *table, int count) {
    uint32_t f = 0;
    time_t now = time(NULL);
    long mem;

    /* F9_RETRY_COUNT / F9_RETRY_WIN */
    if (svc->restart_count < svc->max_restarts)     f |= F9_RETRY_COUNT;
    if (now - svc->last_start >= COOLDOWN_SECS)     f |= F9_RETRY_WIN;

    /* F9_FALL_EXISTS / F9_FALL_HEALTH — no fallback system yet, reserved */
    (void)table; (void)count;

    /* F9_MEM_FREE / F9_MEM_SUFF */
    mem = free_mem_kb();
    if (mem > 0)            f |= F9_MEM_FREE;
    if (mem >= MEM_MIN_KB)  f |= F9_MEM_SUFF;

    /* F9_ESC_PATH / F9_ESC_AUTH — no escalation path yet */

    /* F9_TIMEOUT_WIN / F9_TIMEOUT_EXT */
    if (svc->restart_count < svc->max_restarts) {
        f |= F9_TIMEOUT_WIN;
        f |= F9_TIMEOUT_EXT;
    }

    /* F9_PARTIAL_LOAD / F9_PARTIAL_MIN — not applicable to process services */

    return f;
}

/* ── F6: last chance before excision ───────────────────────────────── */

uint32_t service_probe_f6(service_t *svc) {
    uint32_t f = 0;
    long mem = free_mem_kb();

    /* F6_ERR_PATH / F6_ERR_RES — can we even attempt recovery? */
    if (svc->restart_count < svc->max_restarts) f |= F6_ERR_PATH;
    if (mem >= MEM_MIN_KB)                      f |= F6_ERR_RES;

    /* F6_ROLL_STATE / F6_ROLL_SAFE — no state snapshot yet, reserved */

    /* F6_ESC_LIMIT / F6_ESC_PATTERN */
    if (svc->restart_count < svc->max_restarts) f |= F6_ESC_LIMIT;
    /* assume non-repeating pattern for now */
    f |= F6_ESC_PATTERN;

    return f;
}

/* ── fork + exec ────────────────────────────────────────────────────── */

static void cgroup_assign(service_t *svc, pid_t pid) {
    char path[160];
    int fd;
    char buf[32];
    int n;

    mkdir("/sys/fs/cgroup/schema-init", 0755);
    int sub_fd = open("/sys/fs/cgroup/schema-init/cgroup.subtree_control", O_WRONLY);
    if (sub_fd >= 0) {
        write(sub_fd, "+cpu +memory +cpuset +pids +io", 30);
        close(sub_fd);
    }
    snprintf(svc->cgroup_path, sizeof(svc->cgroup_path),
             "/sys/fs/cgroup/schema-init/%s", svc->name);
    mkdir(svc->cgroup_path, 0755);

    snprintf(path, sizeof(path), "%s/cgroup.procs", svc->cgroup_path);
    fd = open(path, O_WRONLY);
    if (fd < 0) { svc->cgroup_path[0] = '\0'; return; }
    n = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
    write(fd, buf, (size_t)n);
    close(fd);
}

static void cgroup_apply_limits(service_t *svc) {
    char path[160];
    char buf[64];
    int fd;
    int n;

    if (!svc->cgroup_path[0]) return;

    if (svc->cpu_limit_pct > 0 && svc->cpu_limit_pct <= 100) {
        snprintf(path, sizeof(path), "%s/cpu.max", svc->cgroup_path);
        fd = open(path, O_WRONLY);
        if (fd >= 0) {
            n = snprintf(buf, sizeof(buf), "%d 100000\n", svc->cpu_limit_pct * 1000);
            write(fd, buf, (size_t)n);
            close(fd);
        }
    }

    if (svc->mem_limit_mb > 0) {
        snprintf(path, sizeof(path), "%s/memory.max", svc->cgroup_path);
        fd = open(path, O_WRONLY);
        if (fd >= 0) {
            n = snprintf(buf, sizeof(buf), "%ld\n", svc->mem_limit_mb * 1024L * 1024L);
            write(fd, buf, (size_t)n);
            close(fd);
        }
    }

    /* priority -> cgroup v2 cpu.weight (proportional share, default 100).
     * Only takes effect under CPU contention: idle services see no penalty,
     * but when cores are saturated a CRITICAL service (e.g. the display
     * stack or the Leg control loop) wins the scheduler over PERIPHERAL
     * background work. This is the analog of systemd's CPUWeight=. */
    {
        int weight = 100;
        if (svc->priority == PRIO_CRITICAL)        weight = 1000;
        else if (svc->priority == PRIO_PERIPHERAL) weight = 10;
        snprintf(path, sizeof(path), "%s/cpu.weight", svc->cgroup_path);
        fd = open(path, O_WRONLY);
        if (fd >= 0) {
            n = snprintf(buf, sizeof(buf), "%d\n", weight);
            write(fd, buf, (size_t)n);
            close(fd);
        }
    }

    if (svc->cpuset[0]) {
        snprintf(path, sizeof(path), "%s/cpuset.cpus", svc->cgroup_path);
        fd = open(path, O_WRONLY);
        if (fd >= 0) {
            write(fd, svc->cpuset, strlen(svc->cpuset));
            close(fd);
        }
    }

    if (svc->cpuset_partition != PART_MEMBER && svc->cpuset[0]) {
        const char *want =
            (svc->cpuset_partition == PART_ISOLATED) ? "isolated" : "root";
        char part[64];
        ssize_t r;
        int ok;

        /* 1. Reserve these cores in schema-init's exclusive union.
         *    Read-modify-write the live value; the kernel resolves the union
         *    and dedups. Strip the trailing newline; no leading comma when
         *    the existing set is empty. Fail-closed if excl read fills the
         *    buffer — a truncated prefix would corrupt another service's
         *    reservation, so skip the parent write and let step 4 degrade. */
        {
            char excl[512] = {0};
            char merged[640];
            int rfd = open("/sys/fs/cgroup/schema-init/cpuset.cpus.exclusive",
                           O_RDONLY);
            int truncated = 0;
            if (rfd >= 0) {
                r = read(rfd, excl, sizeof(excl) - 1);
                close(rfd);
                if (r > 0) excl[r] = '\0';
                if (r == (ssize_t)(sizeof(excl) - 1)) truncated = 1;
            }
            excl[strcspn(excl, "\n")] = '\0';
            if (excl[0])
                snprintf(merged, sizeof(merged), "%s,%s", excl, svc->cpuset);
            else
                snprintf(merged, sizeof(merged), "%s", svc->cpuset);
            if (truncated) {
                fprintf(stderr,
                        "[schema-init] HAZARD: '%s' parent cpuset.cpus.exclusive"
                        " too large to extend safely — skipping parent write,"
                        " partition will degrade to plain pinning\n",
                        svc->name);
            } else {
                fd = open("/sys/fs/cgroup/schema-init/cpuset.cpus.exclusive",
                          O_WRONLY);
                if (fd >= 0) { write(fd, merged, strlen(merged)); close(fd); }
            }
        }

        /* 2. Claim the cores exclusively for this service. */
        snprintf(path, sizeof(path), "%s/cpuset.cpus.exclusive", svc->cgroup_path);
        fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, svc->cpuset, strlen(svc->cpuset)); close(fd); }

        /* 3. Request the partition. */
        snprintf(path, sizeof(path), "%s/cpuset.cpus.partition", svc->cgroup_path);
        fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, want, strlen(want)); close(fd); }

        /* 4. Read back; the kernel appends " invalid (...)" on failure. */
        part[0] = '\0';
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            r = read(fd, part, sizeof(part) - 1);
            close(fd);
            if (r > 0) part[r] = '\0';
        }
        ok = (strstr(part, "invalid") == NULL &&
              strncmp(part, want, strlen(want)) == 0);

        if (!ok) {
            /* Degrade: revert to plain member pinning (cpuset.cpus stands).
             * No parent rollback — a member child claims no cores, so the
             * leftover union entry is inert. */
            fd = open(path, O_WRONLY);                 /* still cpuset.cpus.partition */
            if (fd >= 0) { write(fd, "member", 6); close(fd); }
            snprintf(path, sizeof(path), "%s/cpuset.cpus.exclusive",
                     svc->cgroup_path);
            fd = open(path, O_WRONLY);
            if (fd >= 0) { write(fd, "\n", 1); close(fd); }
            part[strcspn(part, "\n")] = '\0';
            fprintf(stderr,
                    "[schema-init] HAZARD: '%s' cpuset_partition=%s rejected "
                    "(kernel: '%s') — degraded to plain cpuset pinning on %s\n",
                    svc->name, want, part[0] ? part : "?", svc->cpuset);
        }
    }
}

int service_spawn(service_t *svc) {
    int sync[2];
    pid_t pid;

    svc->ready_path_verified = 0;

    if (pipe(sync) < 0) return -1;

    if (svc->allowed_slot_min >= 0) {
        char *slot_env = getenv("SLOT_ID");
        int slot_id = slot_env ? atoi(slot_env) : -1;
        if (slot_id < svc->allowed_slot_min || slot_id > svc->allowed_slot_max) {
            fprintf(stderr,
                    "[schema-init] HAZARD: '%s' slot constraint violation "
                    "(SLOT_ID=%s, allowed=[%d,%d]) — spawn refused\n",
                    svc->name,
                    slot_env ? slot_env : "unset",
                    svc->allowed_slot_min, svc->allowed_slot_max);
            close(sync[0]); close(sync[1]);
            svc->flags |= SVC_NO_RESTART;
            return -1;
        }
    }

    pid = fork();
    if (pid < 0) { close(sync[0]); close(sync[1]); return -1; }

    if (pid == 0) {
        char c;
        service_reset_child_sigmask();
        setsid();
        close(sync[1]);
        read(sync[0], &c, 1);
        close(sync[0]);
        char log_path[256];
        mkdir("/var/log/schema-init", 0755);
        snprintf(log_path, sizeof(log_path), "/var/log/schema-init/%s.log", svc->name);
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
        if (fd < 0) {
            mkdir("/run/log", 0755);
            mkdir("/run/log/schema-init", 0755);
            snprintf(log_path, sizeof(log_path), "/run/log/schema-init/%s.log", svc->name);
            fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
        }
        if (fd < 0) {
            fd = open("/dev/null", O_WRONLY);
        }
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
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
        if (svc->priority == PRIO_CRITICAL) {
            if (setpriority(PRIO_PROCESS, 0, -10) < 0) {
                fprintf(stderr, "[schema-init] Warning: failed to set critical priority for %s: %s\n", svc->name, strerror(errno));
            }
        } else if (svc->priority == PRIO_PERIPHERAL) {
            if (setpriority(PRIO_PROCESS, 0, 10) < 0) {
                fprintf(stderr, "[schema-init] Warning: failed to set peripheral priority for %s: %s\n", svc->name, strerror(errno));
            }
        }
        if (svc->run_uid) {
            char xdg[48];
            snprintf(xdg, sizeof(xdg), "/run/user/%u", (unsigned)svc->run_uid);
            mkdir(xdg, 0700);
            chown(xdg, svc->run_uid, svc->run_gid);
            setenv("XDG_RUNTIME_DIR", xdg, 1);
            initgroups(svc->run_user[0] ? svc->run_user : "nobody", svc->run_gid);
            setgid(svc->run_gid);
            setuid(svc->run_uid);
        }
        execv(svc->exec, svc->argv);
        _exit(127);
    }

    close(sync[0]);
    svc->child_pid  = pid;
    svc->last_start = time(NULL);
    svc->start_time = svc->last_start;
    clock_gettime(CLOCK_MONOTONIC, &svc->spawn_time_mono);
    svc->last_pet   = svc->spawn_time_mono;
    svc->restart_count++;
    cgroup_assign(svc, pid);
    /* cgroup v2 partition order: child cpuset.cpus → parent cpuset.cpus.exclusive
     * → child cpuset.cpus.exclusive → child cpuset.cpus.partition; any other
     * order breaks partition formation. */
    cgroup_apply_limits(svc);
    write(sync[1], "", 1);
    close(sync[1]);
    return 0;
}

/* cpuset.cpus list ("0-3,7,11") → bitmap; tolerant of spaces/newlines. */
static void cpulist_parse(const char *s, unsigned char *bits, int nbytes) {
    while (*s) {
        int a, b, i;
        while (*s == ',' || *s == ' ' || *s == '\n') s++;
        if (!*s) break;
        a = b = atoi(s);
        while (*s && *s != ',' && *s != '-') s++;
        if (*s == '-') { s++; b = atoi(s); while (*s && *s != ',') s++; }
        for (i = a; i <= b && i / 8 < nbytes; i++) bits[i / 8] |= 1u << (i % 8);
    }
}

/* bitmap → compact cpuset.cpus list, collapsing runs into "a-b". */
static void cpulist_emit(const unsigned char *bits, int nbits,
                         char *out, size_t outsz) {
    size_t off = 0;
    int i = 0;
    out[0] = '\0';
    while (i < nbits) {
        int j;
        if (!(bits[i / 8] & (1u << (i % 8)))) { i++; continue; }
        j = i;
        while (j + 1 < nbits && (bits[(j + 1) / 8] & (1u << ((j + 1) % 8)))) j++;
        if (off && off < outsz) off += snprintf(out + off, outsz - off, ",");
        if (off < outsz) {
            if (i == j) off += snprintf(out + off, outsz - off, "%d", i);
            else        off += snprintf(out + off, outsz - off, "%d-%d", i, j);
        }
        i = j + 1;
    }
}

void service_cgroup_kill(service_t *svc) {
    char path[160];
    int fd;

    if (!svc->cgroup_path[0]) return;

    /* Release a partition reservation before destroying the cgroup. An
     * isolated/root child carves its cores out of general scheduling via
     * schema-init's cpuset.cpus.exclusive union (see cgroup_apply_limits);
     * rmdir alone leaves that union stale, so the cores never return until
     * reboot and the union grows unbounded across restarts. Undo it in the
     * reverse order of the apply: demote the child to member so it stops
     * claiming the cores, then subtract them from the parent union. */
    if (svc->cpuset_partition != PART_MEMBER && svc->cpuset[0]) {
        const char *excl_path =
            "/sys/fs/cgroup/schema-init/cpuset.cpus.exclusive";
        unsigned char have[128] = {0}, mine[128] = {0};
        char excl[512] = {0}, out[512];
        ssize_t r;
        int k;

        snprintf(path, sizeof(path), "%s/cpuset.cpus.partition",
                 svc->cgroup_path);
        fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, "member", 6); close(fd); }

        fd = open(excl_path, O_RDONLY);
        if (fd >= 0) {
            r = read(fd, excl, sizeof(excl) - 1);
            close(fd);
            if (r > 0) excl[r] = '\0';
        }
        excl[strcspn(excl, "\n")] = '\0';
        cpulist_parse(excl, have, sizeof(have));
        cpulist_parse(svc->cpuset, mine, sizeof(mine));
        for (k = 0; k < (int)sizeof(have); k++) have[k] &= ~mine[k];
        cpulist_emit(have, (int)sizeof(have) * 8, out, sizeof(out));
        fd = open(excl_path, O_WRONLY);
        if (fd >= 0) {
            if (out[0]) write(fd, out, strlen(out));
            else        write(fd, "\n", 1);   /* empty the union */
            close(fd);
        }
    }

    /* Linux 5.14+: write 1 to cgroup.kill nukes the whole subtree */
    snprintf(path, sizeof(path), "%s/cgroup.kill", svc->cgroup_path);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, "1", 1);
        close(fd);
    } else {
        /* fallback: read cgroup.procs and kill each PID individually */
        FILE *f;
        pid_t p;
        snprintf(path, sizeof(path), "%s/cgroup.procs", svc->cgroup_path);
        f = fopen(path, "r");
        if (f) {
            while (fscanf(f, "%d", &p) == 1)
                kill(p, SIGKILL);
            fclose(f);
        }
    }

    rmdir(svc->cgroup_path);
    svc->cgroup_path[0] = '\0';
}

/* ── logging ─────────────────────────────────────────────────────────  */

/* The console is the rail's only witness, and on hardware it scrolls away --
 * every marker vmtest greps off the serial line was unrecoverable after boot.
 * Mirror each line into rail.log.
 *
 * Resolve and open the path on EVERY call rather than caching the fd. PID 1
 * emits its first markers before it has mounted the /run tmpfs, so a cached fd
 * opened that early keeps writing into the file it created on the underlying
 * rootfs -- which the tmpfs then hides. The rail looked healthy and the log was
 * unreachable. Rail events are boot-rate, so the reopen costs nothing, and it
 * makes logrotate's copytruncate a non-issue too. */
static void rail_write(const char *line, size_t len) {
    int fd;

    mkdir("/var/log/schema-init", 0755);
    fd = open("/var/log/schema-init/rail.log",
              O_WRONLY | O_CREAT | O_APPEND, 0640);
    if (fd < 0) {
        mkdir("/run/log", 0755);
        mkdir("/run/log/schema-init", 0755);
        fd = open("/run/log/schema-init/rail.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0640);
    }
    if (fd < 0) return;
    (void)!write(fd, line, len);
    close(fd);
}

void service_log(const service_t *svc, const char *event) {
    char line[256];
    int n;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    n = snprintf(line, sizeof(line),
                 "[%02d:%02d:%02d] %-20s  %-12s  state=%-12s  wt=%d  pid=%d\n",
                 t->tm_hour, t->tm_min, t->tm_sec,
                 svc->name, event,
                 state_name(svc->inst.state),
                 svc->inst.weight,
                 (int)svc->child_pid);
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;

    fputs(line, stdout);
    fflush(stdout);

    rail_write(line, (size_t)n);
}

/* ── dependency readiness ───────────────────────────────────────────── */

int service_deps_ready(service_t *svc, service_t *stable, int scount,
                       const uint8_t *grp_states, int gcount) {
    int i;
    for (i = 0; i < MAX_DEPS; i++) {
        int di = svc->dep_idx[i];
        int gi = svc->grp_dep_idx[i];
        uint8_t s;

        if (!svc->dep_name[i][0]) break;

        if (di >= 0) {
            if (di >= scount) return 0;
            s = stable[di].inst.state;
            if (s == STATE_EXCISED) {
                if (stable[di].flags & SVC_CRITICAL) return 0;
                continue;   /* non-critical excised dep: proceed without it */
            }
            if (s != STATE_FUNDAMENTAL && s != STATE_SETTLED &&
                s != STATE_PERFECT)                              return 0;
        }

        if (gi >= 0) {
            if (gi >= gcount) return 0;
            s = grp_states[gi];
            if (s == STATE_EXCISED)                              return 0;
            if (s != STATE_FUNDAMENTAL && s != STATE_SETTLED &&
                s != STATE_PERFECT)                              return 0;
        }
    }
    return 1;
}

static uint32_t fnv1a_file(const char *path) {
    uint32_t h = 2166136261u;
    FILE *f = fopen(path, "r");
    int c;
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF) {
        h ^= (uint32_t)(unsigned char)c;
        h *= 16777619u;
    }
    fclose(f);
    return h;
}

/* ── service file parser ─────────────────────────────────────────────
 *
 * Simple format — one key=value per line:
 *   name=sshd
 *   exec=/usr/sbin/sshd
 *   args=-D
 *   dep=dbus
 *   oneshot=0
 *   needs_root=1
 *   critical=0
 */

static int parse_partition(const char *val) {
    if (strcasecmp(val, "isolated") == 0) return PART_ISOLATED;
    if (strcasecmp(val, "root") == 0)     return PART_ROOT;
    return PART_MEMBER;
}

/* parse on_calendar=HH:MM into svc->timer_cal_{hour,min} and set the timer
 * flags. Returns 0 on success, -1 on malformed input (caller leaves the
 * service non-timer so a typo can't silently schedule garbage). */
static int parse_calendar(service_t *svc, const char *val) {
    int h = -1, m = -1;
    char extra;
    if (sscanf(val, "%d:%d%c", &h, &m, &extra) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    svc->timer_cal_hour = h;
    svc->timer_cal_min  = m;
    svc->flags |= SVC_TIMER | SVC_TIMER_CALENDAR | SVC_ONESHOT;
    return 0;
}

int services_load(const char *dir, service_t *table, int max) {
    DIR *d = opendir(dir);
    struct dirent *ent;
    int count = 0;
    int warned = 0;

    if (!d) return 0;

    while ((ent = readdir(d))) {
        char path[512];
        FILE *f;
        char line[512];
        service_t *svc;
        int argc;
        size_t nlen = strlen(ent->d_name);

        /* only .svc files */
        if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".svc") != 0)
            continue;

        if (count >= max) {
            if (!warned) {
                fprintf(stderr, "schema-init: WARNING — MAX_SERVICES (%d) reached; extra .svc files ignored\n", max);
                warned = 1;
            }
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        f = fopen(path, "r");
        if (!f) continue;

        svc = &table[count];
        memset(svc, 0, sizeof(*svc));
        for (int i = 0; i < MAX_DEPS; i++) svc->dep_idx[i] = -1;
        for (int i = 0; i < MAX_DEPS; i++) svc->grp_dep_idx[i] = -1;
        schema_instance_init(&svc->inst, 0, STATE_PERFECT);
        svc->stable_secs = STABLE_SECS;
        svc->priority = PRIO_STANDARD;
        svc->start_timeout_sec = -1;
        svc->allowed_slot_min = -1;
        svc->allowed_slot_max = -1;
        svc->max_restarts = MAX_RESTARTS;
        svc->timer_cal_hour = -1;
        int dep_slot = 0;

        argc = 0;
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            char *val;
            if (!eq) continue;
            *eq = 0;
            val = eq + 1;
            /* strip newline */
            val[strcspn(val, "\r\n")] = 0;

            if (strcmp(line, "name") == 0)
                strncpy(svc->name, val, sizeof(svc->name) - 1);
            else if (strcmp(line, "exec") == 0) {
                strncpy(svc->exec, val, sizeof(svc->exec) - 1);
                svc->argv[0] = svc->exec;
                argc = 1;
            } else if (strcmp(line, "args") == 0 && argc < MAX_ARGV - 1) {
                svc->argv[argc++] = strdup(val);
            } else if (strcmp(line, "dep") == 0 && dep_slot < MAX_DEPS) {
                strncpy(svc->dep_name[dep_slot++], val, 63);
            } else if (strcmp(line, "oneshot") == 0 && atoi(val))
                svc->flags |= SVC_ONESHOT;
            else if (strcmp(line, "needs_root") == 0 && atoi(val))
                svc->flags |= SVC_NEEDS_ROOT;
            else if (strcmp(line, "critical") == 0 && atoi(val))
                svc->flags |= SVC_CRITICAL;
            else if (strcmp(line, "no_restart") == 0 && atoi(val))
                svc->flags |= SVC_NO_RESTART;
            else if (strcmp(line, "stable_secs") == 0 && (atoi(val) > 0 || strcmp(val, "0") == 0))
                svc->stable_secs = atoi(val);
            else if (strcmp(line, "ready_path") == 0)
                strncpy(svc->ready_path, val, sizeof(svc->ready_path) - 1);
            else if (strcmp(line, "priority") == 0) {
                if (strcasecmp(val, "critical") == 0) svc->priority = PRIO_CRITICAL;
                else if (strcasecmp(val, "peripheral") == 0) svc->priority = PRIO_PERIPHERAL;
                else svc->priority = PRIO_STANDARD;
            } else if (strcmp(line, "fuse") == 0) {
                svc->fuse = atoi(val);
            } else if (strcmp(line, "fuse_cmd") == 0) {
                strncpy(svc->fuse_cmd, val, sizeof(svc->fuse_cmd) - 1);
            } else if (strcmp(line, "failsafe") == 0) {
                strncpy(svc->failsafe_cmd, val, sizeof(svc->failsafe_cmd) - 1);
            } else if (strcmp(line, "failsafe_timeout_ms") == 0) {
                svc->failsafe_timeout_ms = atoi(val);
            } else if (strcmp(line, "ready_poll_hz") == 0) {
                svc->ready_poll_hz = atoi(val);
            } else if (strcmp(line, "no_excise") == 0) {
                svc->no_excise = atoi(val);
            } else if (strcmp(line, "watchdog_timeout_ms") == 0) {
                svc->watchdog_timeout_ms = atoi(val);
            } else if (strcmp(line, "cpu_limit") == 0) {
                svc->cpu_limit_pct = atoi(val);
            } else if (strcmp(line, "mem_limit") == 0) {
                svc->mem_limit_mb = atol(val);
            } else if (strcmp(line, "cpuset") == 0) {
                strncpy(svc->cpuset, val, sizeof(svc->cpuset) - 1);
            } else if (strcmp(line, "cpuset_partition") == 0) {
                svc->cpuset_partition = parse_partition(val);
            } else if (strcmp(line, "allowed_slot_min") == 0) {
                svc->allowed_slot_min = atoi(val);
            } else if (strcmp(line, "allowed_slot_max") == 0) {
                svc->allowed_slot_max = atoi(val);
            } else if (strcmp(line, "max_restarts") == 0) {
                svc->max_restarts = atoi(val);
            } else if (strcmp(line, "start_timeout_sec") == 0) {
                svc->start_timeout_sec = atoi(val);
            } else if (strcmp(line, "on_boot_sec") == 0) {
                svc->timer_boot_sec = atoi(val);
                svc->flags |= SVC_TIMER | SVC_ONESHOT;
            } else if (strcmp(line, "on_active_sec") == 0) {
                svc->timer_interval_sec = atoi(val);
                svc->flags |= SVC_TIMER | SVC_ONESHOT;
            } else if (strcmp(line, "on_calendar") == 0) {
                if (parse_calendar(svc, val) != 0)
                    fprintf(stderr, "[schema-init] WARN: '%s' bad on_calendar='%s' "
                            "(want HH:MM 00:00-23:59) — ignoring\n", svc->name, val);
            } else if (strcmp(line, "persistent") == 0) {
                if (atoi(val)) svc->flags |= SVC_TIMER_PERSIST;
            } else if (strcmp(line, "user") == 0) {
                struct passwd *pw = getpwnam(val);
                if (pw) {
                    svc->run_uid = pw->pw_uid;
                    svc->run_gid = pw->pw_gid;
                    strncpy(svc->run_user, val, sizeof(svc->run_user) - 1);
                }
            }
        }
        fclose(f);
        if (svc->cpuset_partition != PART_MEMBER && svc->cpuset[0] == '\0') {
            fprintf(stderr,
                    "[schema-init] WARN: '%s' cpuset_partition set without cpuset= "
                    "— ignoring (no cores to isolate)\n", svc->name);
            svc->cpuset_partition = PART_MEMBER;
        }
        if ((svc->flags & SVC_TIMER_PERSIST) && !(svc->flags & SVC_TIMER_CALENDAR)) {
            fprintf(stderr, "[schema-init] WARN: '%s' persistent=1 needs on_calendar "
                    "— ignoring (catch-up only applies to calendar timers)\n", svc->name);
            svc->flags &= ~SVC_TIMER_PERSIST;
        }
        svc->content_hash = fnv1a_file(path);

        /* default start timeout: protect oneshots (but not timers, which may
         * legitimately run long); daemons rely on stable_secs instead */
        if (svc->start_timeout_sec == -1) {
            svc->start_timeout_sec =
                ((svc->flags & SVC_ONESHOT) && !(svc->flags & SVC_TIMER))
                ? ONESHOT_START_TIMEOUT : 0;
        }

        char base_name[256];
        memset(base_name, 0, sizeof(base_name));
        if (nlen - 4 < sizeof(base_name)) {
            memcpy(base_name, ent->d_name, nlen - 4);
        } else {
            memcpy(base_name, ent->d_name, sizeof(base_name) - 1);
        }
        {
            char *bat = strchr(base_name, '@');
            if (bat && !*(bat + 1)) {
                /* template file itself (motor@.svc) — not a spawnable instance */
                continue;
            }
            if (bat || !svc->name[0]) {
                strncpy(svc->name, base_name, sizeof(svc->name) - 1);
                svc->name[sizeof(svc->name) - 1] = '\0';
            }
            if (bat && *(bat + 1))
                strncpy(svc->instance, bat + 1, sizeof(svc->instance) - 1);
        }

        if (!svc->name[0] || !svc->exec[0]) continue;
        svc->argv[argc] = NULL;
        count++;
    }

    closedir(d);

    /* second pass: resolve dep names → indices */
    {
        int i, d, j;
        for (i = 0; i < count; i++) {
            for (d = 0; d < MAX_DEPS; d++) {
                if (!table[i].dep_name[d][0]) break;
                for (j = 0; j < count; j++) {
                    if (strcmp(table[j].name, table[i].dep_name[d]) == 0) {
                        table[i].dep_idx[d] = j;
                        break;
                    }
                }
                /* unresolved dep name: logged at runtime via service_deps_ready */
            }
        }
    }

    return count;
}

/* ── single service file loader ─────────────────────────────────────── */

int service_load_one(const char *path, service_t *svc) {
    FILE *f;
    char line[512];
    int argc, dep_slot;

    f = fopen(path, "r");
    if (!f) return -1;

    memset(svc, 0, sizeof(*svc));
    for (int i = 0; i < MAX_DEPS; i++) svc->dep_idx[i] = -1;
    for (int i = 0; i < MAX_DEPS; i++) svc->grp_dep_idx[i] = -1;
    schema_instance_init(&svc->inst, 0, STATE_PERFECT);
    svc->stable_secs = STABLE_SECS;
    svc->priority = PRIO_STANDARD;
    svc->start_timeout_sec = -1;
    svc->allowed_slot_min = -1;
    svc->allowed_slot_max = -1;
    svc->max_restarts = MAX_RESTARTS;
    svc->timer_cal_hour = -1;
    argc = 0;
    dep_slot = 0;

    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        char *val;
        if (!eq) continue;
        *eq = 0;
        val = eq + 1;
        val[strcspn(val, "\r\n")] = 0;

        if (strcmp(line, "name") == 0)
            strncpy(svc->name, val, sizeof(svc->name) - 1);
        else if (strcmp(line, "exec") == 0) {
            strncpy(svc->exec, val, sizeof(svc->exec) - 1);
            svc->argv[0] = svc->exec;
            argc = 1;
        } else if (strcmp(line, "args") == 0 && argc < MAX_ARGV - 1)
            svc->argv[argc++] = strdup(val);
        else if (strcmp(line, "dep") == 0 && dep_slot < MAX_DEPS)
            strncpy(svc->dep_name[dep_slot++], val, 63);
        else if (strcmp(line, "oneshot") == 0 && atoi(val))
            svc->flags |= SVC_ONESHOT;
        else if (strcmp(line, "needs_root") == 0 && atoi(val))
            svc->flags |= SVC_NEEDS_ROOT;
        else if (strcmp(line, "critical") == 0 && atoi(val))
            svc->flags |= SVC_CRITICAL;
        else if (strcmp(line, "no_restart") == 0 && atoi(val))
            svc->flags |= SVC_NO_RESTART;
        else if (strcmp(line, "stable_secs") == 0 && (atoi(val) > 0 || strcmp(val, "0") == 0))
            svc->stable_secs = atoi(val);
        else if (strcmp(line, "ready_path") == 0)
            strncpy(svc->ready_path, val, sizeof(svc->ready_path) - 1);
        else if (strcmp(line, "priority") == 0) {
            if (strcasecmp(val, "critical") == 0) svc->priority = PRIO_CRITICAL;
            else if (strcasecmp(val, "peripheral") == 0) svc->priority = PRIO_PERIPHERAL;
            else svc->priority = PRIO_STANDARD;
        } else if (strcmp(line, "fuse") == 0) {
            svc->fuse = atoi(val);
        } else if (strcmp(line, "fuse_cmd") == 0) {
            strncpy(svc->fuse_cmd, val, sizeof(svc->fuse_cmd) - 1);
        } else if (strcmp(line, "failsafe") == 0) {
            strncpy(svc->failsafe_cmd, val, sizeof(svc->failsafe_cmd) - 1);
        } else if (strcmp(line, "failsafe_timeout_ms") == 0) {
            svc->failsafe_timeout_ms = atoi(val);
        } else if (strcmp(line, "ready_poll_hz") == 0) {
            svc->ready_poll_hz = atoi(val);
        } else if (strcmp(line, "no_excise") == 0) {
            svc->no_excise = atoi(val);
        } else if (strcmp(line, "watchdog_timeout_ms") == 0) {
            svc->watchdog_timeout_ms = atoi(val);
        } else if (strcmp(line, "cpu_limit") == 0) {
            svc->cpu_limit_pct = atoi(val);
        } else if (strcmp(line, "mem_limit") == 0) {
            svc->mem_limit_mb = atol(val);
        } else if (strcmp(line, "cpuset") == 0) {
            strncpy(svc->cpuset, val, sizeof(svc->cpuset) - 1);
        } else if (strcmp(line, "cpuset_partition") == 0) {
            svc->cpuset_partition = parse_partition(val);
        } else if (strcmp(line, "allowed_slot_min") == 0) {
            svc->allowed_slot_min = atoi(val);
        } else if (strcmp(line, "allowed_slot_max") == 0) {
            svc->allowed_slot_max = atoi(val);
        } else if (strcmp(line, "max_restarts") == 0) {
            svc->max_restarts = atoi(val);
        } else if (strcmp(line, "start_timeout_sec") == 0) {
            svc->start_timeout_sec = atoi(val);
        } else if (strcmp(line, "on_boot_sec") == 0) {
            svc->timer_boot_sec = atoi(val);
            svc->flags |= SVC_TIMER | SVC_ONESHOT;
        } else if (strcmp(line, "on_active_sec") == 0) {
            svc->timer_interval_sec = atoi(val);
            svc->flags |= SVC_TIMER | SVC_ONESHOT;
        } else if (strcmp(line, "on_calendar") == 0) {
            if (parse_calendar(svc, val) != 0)
                fprintf(stderr, "[schema-init] WARN: '%s' bad on_calendar='%s' "
                        "(want HH:MM 00:00-23:59) — ignoring\n", svc->name, val);
        } else if (strcmp(line, "persistent") == 0) {
            if (atoi(val)) svc->flags |= SVC_TIMER_PERSIST;
        } else if (strcmp(line, "user") == 0) {
            struct passwd *pw = getpwnam(val);
            if (pw) { svc->run_uid = pw->pw_uid; svc->run_gid = pw->pw_gid; }
        }
    }
    fclose(f);
    if (svc->cpuset_partition != PART_MEMBER && svc->cpuset[0] == '\0') {
        fprintf(stderr,
                "[schema-init] WARN: '%s' cpuset_partition set without cpuset= "
                "— ignoring (no cores to isolate)\n", svc->name);
        svc->cpuset_partition = PART_MEMBER;
    }

    if ((svc->flags & SVC_TIMER_PERSIST) && !(svc->flags & SVC_TIMER_CALENDAR)) {
        fprintf(stderr, "[schema-init] WARN: '%s' persistent=1 needs on_calendar "
                "— ignoring (catch-up only applies to calendar timers)\n", svc->name);
        svc->flags &= ~SVC_TIMER_PERSIST;
    }

    if (svc->start_timeout_sec == -1) {
        svc->start_timeout_sec =
            ((svc->flags & SVC_ONESHOT) && !(svc->flags & SVC_TIMER))
            ? ONESHOT_START_TIMEOUT : 0;
    }

    char base_name[256];
    memset(base_name, 0, sizeof(base_name));
    const char *fname = strrchr(path, '/');
    if (fname) fname++;
    else fname = path;
    size_t flen = strlen(fname);
    if (flen >= 5 && strcmp(fname + flen - 4, ".svc") == 0) {
        size_t blen = flen - 4;
        if (blen >= sizeof(base_name)) blen = sizeof(base_name) - 1;
        memcpy(base_name, fname, blen);
        if (strchr(base_name, '@') || !svc->name[0]) {
            strncpy(svc->name, base_name, sizeof(svc->name) - 1);
            svc->name[sizeof(svc->name) - 1] = '\0';
        }
    }

    if (!svc->name[0] || !svc->exec[0]) return -1;
    svc->argv[argc] = NULL;
    return 0;
}

/* ── dependency cycle detection (DFS, three-color) ──────────────────── */

#define COLOR_WHITE 0
#define COLOR_GRAY  1
#define COLOR_BLACK 2

static int dfs(int node, int *color, service_t *table, int count, int *cycle_found) {
    int i;
    color[node] = COLOR_GRAY;

    for (i = 0; i < MAX_DEPS; i++) {
        int dep = table[node].dep_idx[i];
        if (dep < 0) continue;
        if (dep >= count) continue;
        if (color[dep] == COLOR_GRAY) {
            fprintf(stderr, "schema-init: dependency cycle: %s -> %s\n",
                    table[node].name, table[dep].name);
            (*cycle_found)++;
        } else if (color[dep] == COLOR_WHITE) {
            dfs(dep, color, table, count, cycle_found);
        }
    }

    color[node] = COLOR_BLACK;
    return 0;
}

int services_check_cycles(service_t *table, int count) {
    int color[MAX_SERVICES] = {0};
    int cycles = 0;
    int i;

    for (i = 0; i < count; i++) {
        if (color[i] == COLOR_WHITE)
            dfs(i, color, table, count, &cycles);
    }

    if (cycles == 0)
        printf("[schema-init] dependency graph: no cycles\n");

    return cycles;
}
