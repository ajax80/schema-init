#include "service.h"
#include "notify.h"
#include "fdstore.h"

fdstore_ent *fdstore_v;
int fdstore_n, fdstore_cap;
#include "caps.h"
#include "ns.h"
#include <linux/capability.h>
#include "coredump.h"
#include "recorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/random.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stddef.h>
#include <glob.h>
#include <sys/statvfs.h>
#include "svc_dropins.h"

static void svc_make_exec_dirs(const service_t *svc, int create);

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

/* The soft RLIMIT_NOFILE PID 1 was handed by the kernel, before we raise it.
 * Zero until service_raise_pid1_nofile() has run. */
static rlim_t nofile_soft_at_boot = 0;

/* PID 1 inherits the kernel default -- 1024 soft against a 4096 hard limit on
 * blakbox. PID 1 itself needs almost none of that (5 open at 49 services,
 * because a service's log fd is opened in the CHILD before exec, not held
 * here), so this is not about capacity. It is about surviving a leak: the
 * control-socket accept path reached EMFILE once already, and at that point
 * PID 1 can no longer open a log, accept a client, or spawn anything -- with
 * no supervisor above it to recover. Raising soft to the hard limit costs
 * nothing and buys 4x the runway.
 *
 * Only the soft limit moves, and only for PID 1: see
 * service_restore_child_nofile(). */
void service_raise_pid1_nofile(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    nofile_soft_at_boot = rl.rlim_cur;
    if (rl.rlim_cur >= rl.rlim_max) return;
    rl.rlim_cur = rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
        fprintf(stderr, "schema-init: could not raise RLIMIT_NOFILE: %s\n",
                strerror(errno));
}

/* A re-exec inherits the raised limit: the new image takes the boot value
 * from the state blob instead of recording the raised one. */
rlim_t service_nofile_soft_at_boot(void) { return nofile_soft_at_boot; }
void service_set_nofile_soft_at_boot(rlim_t v) { nofile_soft_at_boot = v; }

/* Hand children back the soft limit PID 1 started with, between fork and exec.
 * Limits are inherited, and a raised soft NOFILE is not a free gift: anything
 * still using select()/fd_set breaks on a descriptor >= FD_SETSIZE (1024), and
 * it fails by corrupting the caller's stack rather than returning an error.
 * systemd keeps a low soft limit and a high hard limit for exactly this
 * reason; a service that wants more can raise its own. */
void service_restore_child_nofile(void) {
    struct rlimit rl;
    if (nofile_soft_at_boot == 0) return;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    rl.rlim_cur = nofile_soft_at_boot;
    setrlimit(RLIMIT_NOFILE, &rl);
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
        if (table[di].inst.state == STATE_EXCISED ||
            table[di].inst.state == STATE_DORMANT) {
            if (table[di].flags & SVC_CRITICAL) { dep_ok = 0; break; }
            continue;
        }
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
    struct timespec now;
    long mem;

    /* F9_RETRY_COUNT / F9_RETRY_WIN — monotonic: an NTP step can't stretch it */
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (svc->restart_count < svc->max_restarts)     f |= F9_RETRY_COUNT;
    if ((!svc->spawn_time_mono.tv_sec && !svc->spawn_time_mono.tv_nsec) ||
        now.tv_sec - svc->spawn_time_mono.tv_sec >= COOLDOWN_SECS)
        f |= F9_RETRY_WIN;

    /* F9_FALL_EXISTS / F9_FALL_HEALTH — a card=fallback: service, and whether it is up */
    service_t *fb = service_fallback(svc, table, count);
    if (fb) {
        f |= F9_FALL_EXISTS;
        if (fb->child_pid > 0 && (fb->inst.state == STATE_FUNDAMENTAL || fb->inst.state == STATE_PERFECT))
            f |= F9_FALL_HEALTH;
    }

    /* F9_MEM_FREE / F9_MEM_SUFF */
    mem = free_mem_kb();
    if (mem > 0)            f |= F9_MEM_FREE;
    if (mem >= MEM_MIN_KB)  f |= F9_MEM_SUFF;

    /* F9_ESC_PATH / F9_ESC_AUTH — a declared card is still in hand */
    if (svc->card_next < svc->card_count) f |= F9_ESC_PATH | F9_ESC_AUTH;

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

const char *service_cgroup_root = "/sys/fs/cgroup/schema-init";
const char *service_notify_sock = NOTIFY_SOCK_PATH;
const char *service_log_dir;

const char *service_cgroup_path_for(const service_t *svc) {
    static char buf[sizeof svc->cgroup_path];
    snprintf(buf, sizeof buf, "%s/%s", service_cgroup_root, svc->name);
    return buf;
}

static void cgroup_assign(service_t *svc, pid_t pid) {
    char path[320];
    int fd;
    char buf[32];
    int n;

    /* the user supervisor lives in <root>/init.scope; a service of that name
     * would share its cgroup, and stopping it would kill the supervisor */
    if (strcmp(svc->name, "init.scope") == 0) {
        svc->cgroup_path[0] = '\0';
        return;
    }
    mkdir(service_cgroup_root, 0755);
    snprintf(path, sizeof(path), "%s/cgroup.subtree_control", service_cgroup_root);
    int sub_fd = open(path, O_WRONLY);
    if (sub_fd >= 0) {
        write(sub_fd, "+cpu +memory +cpuset +pids +io", 30);
        close(sub_fd);
    }
    snprintf(svc->cgroup_path, sizeof(svc->cgroup_path), "%s",
             service_cgroup_path_for(svc));
    /* a previous run's nested cgroups (still there if its processes were
     * slow to die) make <svc> a non-leaf, and the pid write below would fail */
    cgroup_rmdir_tree(svc->cgroup_path);
    mkdir(svc->cgroup_path, 0755);

    snprintf(path, sizeof(path), "%s/cgroup.procs", svc->cgroup_path);
    fd = open(path, O_WRONLY);
    if (fd < 0) { svc->cgroup_path[0] = '\0'; return; }
    n = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
    if (write(fd, buf, (size_t)n) != n) {
        /* a previous run's nested cgroups still populated: <svc> is not a leaf */
        fprintf(stderr, "[schema-init] %s: cannot join %s: %s\n", svc->name, svc->cgroup_path, strerror(errno));
        svc->cgroup_path[0] = '\0';
    }
    close(fd);
}

/* best-effort single-value cgroup write: silently ignores a missing file (the
 * controller isn't enabled on this cgroup) or a value the kernel rejects. */
static void cg_write(const char *cgroup_path, const char *file, const char *val) {
    char path[320];
    int fd;
    snprintf(path, sizeof(path), "%s/%s", cgroup_path, file);
    fd = open(path, O_WRONLY);
    if (fd < 0) return;
    write(fd, val, strlen(val));
    close(fd);
}

static void cgroup_apply_limits(service_t *svc) {
    char path[320];
    char buf[64];
    int fd;
    int n;

    if (!svc->cgroup_path[0]) return;

    long quota = cpu_max_quota(svc->cpu_limit_pct);
    if (quota > 0) {
        snprintf(buf, sizeof(buf), "%ld 100000\n", quota);
        cg_write(svc->cgroup_path, "cpu.max", buf);
    }

    if (svc->pids_max > 0) {
        snprintf(buf, sizeof(buf), "%d\n", svc->pids_max);
        cg_write(svc->cgroup_path, "pids.max", buf);
    }

    if (svc->mem_limit_mb > 0) {
        snprintf(path, sizeof(path), "%s/memory.max", svc->cgroup_path);
        fd = open(path, O_WRONLY);
        if (fd >= 0) {
            n = snprintf(buf, sizeof(buf), "%lld\n", svc->mem_limit_mb * 1024LL * 1024LL);
            write(fd, buf, (size_t)n);
            close(fd);
        }
    }

    /* memory.high: soft cap 10% below the hard cap -> the kernel reclaims cold
     * pages under gentle back-pressure before ever hitting the OOM path. Pairs
     * with the PSI reclaim executive. Only when a hard mem_limit exists. */
    {
        long long high = mem_high_bytes(svc->mem_limit_mb);
        if (high > 0) {
            n = snprintf(buf, sizeof(buf), "%lld\n", high);
            cg_write(svc->cgroup_path, "memory.high", buf);
        }
    }

    {
        long long low = mem_low_bytes(svc->mem_low_mb);
        long long sw = swap_max_value(svc->priority, svc->swap_max_set, svc->swap_max_mb);
        if (low > 0) {
            snprintf(buf, sizeof(buf), "%lld\n", low);
            cg_write(svc->cgroup_path, "memory.low", buf);
        }
        if (sw == CG_SWAP_MAX) {
            cg_write(svc->cgroup_path, "memory.swap.max", "max\n");
        } else if (sw >= 0) {
            snprintf(buf, sizeof(buf), "%lld\n", sw);
            cg_write(svc->cgroup_path, "memory.swap.max", buf);
        }
    }

    /* kill a service's whole cgroup together on OOM — no orphaned survivors. */
    cg_write(svc->cgroup_path, "memory.oom.group", "1\n");

    /* priority -> cgroup v2 CPU + IO tiering. Only bites under contention: a
     * CRITICAL service (display stack, audio, the Leg loop) wins the scheduler
     * and the disk over STANDARD; PERIPHERAL background work yields entirely via
     * cpu.idle. io.weight shields CRITICAL disk I/O from peripheral hogs — keeps
     * PipeWire from crackling under frigate/torrent load. */
    {
        cgroup_tier_t tier = cgroup_tiering(svc->priority, svc->cpu_weight);
        if (tier.cpu_idle) {
            cg_write(svc->cgroup_path, "cpu.idle", "1\n");
        } else {
            n = snprintf(buf, sizeof(buf), "%d\n", tier.cpu_weight);
            cg_write(svc->cgroup_path, "cpu.weight", buf);
        }
        n = snprintf(buf, sizeof(buf), "%d\n", tier.io_weight);
        cg_write(svc->cgroup_path, "io.weight", buf);
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
            char excl_path[320];
            snprintf(excl_path, sizeof excl_path, "%s/cpuset.cpus.exclusive",
                     service_cgroup_root);
            int rfd = open(excl_path, O_RDONLY);
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
                fd = open(excl_path, O_WRONLY);
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

static int parse_int_in(const char *v, long lo, long hi, long *out) {
    char *e;
    long n;
    if (!*v) return -1;
    n = strtol(v, &e, 10);
    if (*e || n < lo || n > hi) return -1;
    *out = n;
    return 0;
}

/* nice=, io_class=, io_priority=, sched_policy=, sched_priority=. 0 or -1 */
static int parse_sched_key(service_t *svc, const char *key, const char *val) {
    static const char *const ioc[] = { "none", "realtime", "best-effort", "idle" };
    static const struct { const char *n; int p; } pol[] = {
        { "other", SCHED_OTHER }, { "batch", SCHED_BATCH }, { "idle", SCHED_IDLE },
        { "fifo", SCHED_FIFO }, { "rr", SCHED_RR },
    };
    long n;
    if (!*val) {
        /* a drop-in's empty value clears the key */
        if (!strcmp(key, "nice")) svc->nice_set = 0;
        else if (!strcmp(key, "io_class")) svc->io_class = -1;
        else if (!strcmp(key, "io_priority")) svc->io_prio = -1;
        else if (!strcmp(key, "sched_policy")) svc->sched_policy = -1;
        else svc->sched_prio = 0;
        return 0;
    }
    if (!strcmp(key, "nice")) {
        if (parse_int_in(val, -20, 19, &n)) return -1;
        svc->nice_val = (int8_t)n;
        svc->nice_set = 1;
    } else if (!strcmp(key, "io_class")) {
        for (n = 0; n < 4 && strcmp(val, ioc[n]); n++) {}
        if (n == 4) return -1;
        svc->io_class = (int8_t)n;
    } else if (!strcmp(key, "io_priority")) {
        if (parse_int_in(val, 0, 7, &n)) return -1;
        svc->io_prio = (int8_t)n;
    } else if (!strcmp(key, "sched_policy")) {
        for (n = 0; n < 5 && strcmp(val, pol[n].n); n++) {}
        if (n == 5) return -1;
        svc->sched_policy = (int8_t)pol[n].p;
    } else {
        if (parse_int_in(val, 1, 99, &n)) return -1;
        svc->sched_prio = (uint8_t)n;
    }
    return 0;
}

/* watch_path=, make_directory=, directory_mode=, trigger_limit_*=. 0 or -1 */
static int parse_watch_key(service_t *svc, const char *key, const char *val) {
    long n;
    if (!strcmp(key, "watch_path")) {
        if (!*val) { svc->watch_count = 0; return 0; }   /* a drop-in's empty value resets the list */
        if (svc->watch_count >= MAX_WATCH || watch_parse(val, &svc->watch[svc->watch_count]) != 0)
            return -1;
        svc->watch_count++;
    } else if (!strcmp(key, "make_directory")) {
        if (parse_int_in(val, 0, 1, &n)) return -1;
        svc->make_directory = (uint8_t)n;
    } else if (!strcmp(key, "directory_mode")) {
        size_t k = strspn(val, "01234567");
        if (!k || k > 4 || val[k]) return -1;
        n = strtol(val, NULL, 8);
        if (n > 07777) return -1;
        svc->directory_mode = (uint16_t)n;
    } else if (!strcmp(key, "trigger_limit_burst")) {
        if (parse_int_in(val, 1, 1000000, &n)) return -1;
        svc->trigger_burst = (int)n;
    } else {
        if (parse_int_in(val, 1, 86400, &n)) return -1;
        svc->trigger_interval_sec = (int)n;
    }
    return 0;
}

/* 1 = handled, 0 = not a mount-ns key, -1 = bad value (load error). */
static int parse_ns_field(service_t *svc, const char *key, const char *val) {
    int r;
    if (strcmp(key, "private_tmp") == 0) {
        r = parse_ns_bool(val, &svc->ns_private_tmp);
        svc->hard_set |= HARD_PT;
    } else if (strcmp(key, "protect_system") == 0) {
        r = parse_protect_system(val, &svc->ns_protect_system);
        svc->hard_set |= HARD_PS;
    } else if (strcmp(key, "protect_home") == 0) {
        r = parse_ns_bool(val, &svc->ns_protect_home);
        svc->hard_set |= HARD_PH;
    } else {
        int i;
        uint8_t on = 0;
        for (i = 0; i < NS_K_COUNT && strcmp(key, ns_kernel_names[i]); i++) {}
        if (i == NS_K_COUNT) return 0;
        r = parse_ns_bool(val, &on);
        if (on) svc->ns_kernel |= (uint8_t)(1U << i);
        else svc->ns_kernel &= (uint8_t)~(1U << i);
    }
    if (r != 0) {
        fprintf(stderr, "[schema-init] %s: bad value %s=%s\n",
                svc->name[0] ? svc->name : "?", key, val);
        return -1;
    }
    return 1;
}

static int path_under(const char *p, const char *const *dirs, int n) {
    for (int i = 0; i < n; i++)
        if (strncmp(p, dirs[i], strlen(dirs[i])) == 0) return 1;
    return 0;
}

static int hardening_default_on;

void service_set_hardening_default(int on) { hardening_default_on = on; }
int  service_hardening_default(void) { return hardening_default_on; }

int hardening_default_resolve(const char *cmdline, const char *file) {
    static const char key[] = "schema.hardening_default=";
    const char *p = cmdline;
    while (p && (p = strstr(p, key)) != NULL) {
        const char *v = p + sizeof(key) - 1;
        if ((p == cmdline || isspace((unsigned char)p[-1])) &&
            (v[0] == '0' || v[0] == '1') &&
            (v[1] == '\0' || isspace((unsigned char)v[1])))
            return v[0] == '1';
        p = v;
    }
    if (!file) return 0;
    while (isspace((unsigned char)*file)) file++;
    size_t n = strlen(file);
    while (n && isspace((unsigned char)file[n - 1])) n--;
    return n == 2 && strncmp(file, "on", 2) == 0;
}

static int modload_cmp(const void *a, const void *b) {
    return strcmp(strrchr(*(const char *const *)a, '/'), strrchr(*(const char *const *)b, '/'));
}

int modload_conf_list(const char *const *dirs, const char **out, int max, int *dropped) {
    static char all[MODLOAD_CONF_MAX][MODLOAD_PATH_MAX];
    static unsigned char masked[MODLOAD_CONF_MAX];
    dev_t ddev[16];
    ino_t dino[16];
    int n = 0, k = 0, nd = 0;
    *dropped = 0;
    for (int d = 0; dirs[d]; d++) {
        char pat[MODLOAD_PATH_MAX];
        struct stat ds;
        glob_t g;
        int dup = 0;
        if (stat(dirs[d], &ds) != 0 || !S_ISDIR(ds.st_mode)) continue;
        /* usrmerge: /lib is /usr/lib, read it once */
        for (int j = 0; j < nd; j++)
            if (ddev[j] == ds.st_dev && dino[j] == ds.st_ino) dup = 1;
        if (dup) continue;
        if (nd < 16) { ddev[nd] = ds.st_dev; dino[nd] = ds.st_ino; nd++; }
        snprintf(pat, sizeof pat, "%s/*.conf", dirs[d]);
        if (glob(pat, 0, NULL, &g) != 0) continue;
        for (size_t i = 0; i < g.gl_pathc; i++) {
            const char *p = g.gl_pathv[i], *base = strrchr(p, '/');
            struct stat st;
            int seen = 0, m;
            /* first directory wins: dirs[] is in priority order */
            for (int j = 0; j < n && !seen; j++)
                seen = !strcmp(strrchr(all[j], '/'), base);
            if (seen || stat(p, &st) != 0) continue;
            m = S_ISCHR(st.st_mode) && st.st_rdev == makedev(1, 3);
            if (!m && !S_ISREG(st.st_mode)) continue;
            if (n == MODLOAD_CONF_MAX || strlen(p) >= MODLOAD_PATH_MAX) { (*dropped)++; continue; }
            memcpy(all[n], p, strlen(p) + 1);
            masked[n++] = m;
        }
        globfree(&g);
    }
    for (int j = 0; j < n; j++) {
        if (masked[j]) continue;
        if (k == max) { (*dropped)++; continue; }
        out[k++] = all[j];
    }
    qsort(out, k, sizeof out[0], modload_cmp);
    return k;
}

int cmdline_modules_load(const char *cl, char (*names)[256], int max) {
    int n = 0;
    while (cl && *cl) {
        size_t w;
        while (isspace((unsigned char)*cl)) cl++;
        w = strcspn(cl, " \t\n");
        if (w > 13 && (!strncmp(cl, "modules_load=", 13) || !strncmp(cl, "modules-load=", 13))) {
            const char *v = cl + 13, *end = cl + w;
            while (v < end) {
                size_t l = strcspn(v, ",");
                if (v + l > end) l = end - v;
                if (l && l < 256 && n < max) {
                    memcpy(names[n], v, l);
                    names[n++][l] = '\0';
                }
                v += l + 1;
            }
        }
        cl += w;
    }
    return n;
}

const char *watchdog_arm_reason(const char *cmdline, int conf_present,
                                const service_t *svcs, int n) {
    static const char key[] = "schema.watchdog=";
    const char *p = cmdline;
    int flag = -1;
    /* the last one wins: a bootloader appends to a baked-in default */
    while (p && (p = strstr(p, key)) != NULL) {
        const char *v = p + sizeof(key) - 1;
        if ((p == cmdline || isspace((unsigned char)p[-1])) &&
            (v[0] == '0' || v[0] == '1') &&
            (v[1] == '\0' || isspace((unsigned char)v[1])))
            flag = v[0] == '1';
        p = v;
    }
    if (flag >= 0) return flag ? "schema.watchdog=1 on the kernel command line" : NULL;
    if (conf_present) return "/etc/schema-init/watchdog exists";
    for (int i = 0; i < n; i++)
        if (svcs[i].watchdog_timeout_ms > 0) return "a service sets watchdog_timeout_ms";
    return NULL;
}

/* Sum, not "max": under memory_recursiveprot (systemd's initrd mounts with it)
 * a root holding max hands the unclaimed rest to every service, and nothing
 * would be reclaimed before anything else. */
void service_cgroup_root_low(const service_t *svcs, int n) {
    long long sum = 0;
    char buf[32];
    if (!service_cgroup_root[0]) return;
    /* each term is at most 2^60, so the sum can't wrap before the check */
    for (int i = 0; i < n && sum <= MEM_LIMIT_MB_MAX * 1048576LL; i++) sum += mem_low_bytes(svcs[i].mem_low_mb);
    mkdir(service_cgroup_root, 0755);
    if (sum > MEM_LIMIT_MB_MAX * 1048576LL) snprintf(buf, sizeof buf, "max\n");
    else snprintf(buf, sizeof buf, "%lld\n", sum);
    cg_write(service_cgroup_root, "memory.low", buf);
}

/* dynamic_user=1 implies systemd's sandbox for it, unless the unit says
 * otherwise; marked so status names the source, and droppable like a default
 * when it would hide the exec */
int service_dynamic_users = 1;

static void apply_dynuser_implies(service_t *svc) {
    static const uint8_t bits[] = { HARD_NNP, HARD_PT, HARD_PS, HARD_PH };
    if (!svc->dynamic_user || !service_dynamic_users) return;
    for (int i = 0; i < 4; i++) {
        uint8_t b = bits[i];
        if (svc->hard_set & b) continue;
        if (b == HARD_NNP) svc->flags |= SVC_NO_NEW_PRIVS;
        else if (b == HARD_PT) svc->ns_private_tmp = 1;
        else if (b == HARD_PS) svc->ns_protect_system = PROTECT_SYSTEM_FULL;
        else svc->ns_protect_home = 1;
        svc->hard_default |= b;
        svc->hard_dyn |= b;
    }
}

static void apply_hardening_defaults(service_t *svc) {
    if (!hardening_default_on) return;
    if (!(svc->hard_set & HARD_NNP)) {
        svc->flags |= SVC_NO_NEW_PRIVS;
        svc->hard_default |= HARD_NNP;
    }
    if (!(svc->hard_set & HARD_PT)) {
        svc->ns_private_tmp = 1;
        svc->hard_default |= HARD_PT;
    }
    if (!(svc->hard_set & HARD_PS)) {
        svc->ns_protect_system = PROTECT_SYSTEM_BASE;
        svc->hard_default |= HARD_PS;
    }
    if (!(svc->hard_set & HARD_PH)) {
        svc->ns_protect_home = 1;
        svc->hard_default |= HARD_PH;
    }
}

/* A mount-ns knob that hides the service's own exec or ready_path: an
 * explicit one refuses the load, a defaulted one is dropped. */
static int ns_hides(service_t *svc, uint8_t bit, uint8_t *knob, const char *knob_name,
                    const char *const *dirs, int n) {
    const char *what = NULL;
    if (!*knob) return 0;
    char wd[260] = "";
    if (svc->working_dir[0]) {
        const char *d = svc->working_dir + (svc->working_dir[0] == '-');
        int root = !svc->run_user[0] || !strcmp(svc->run_user, "root");
        snprintf(wd, sizeof wd, "%s/", strcmp(d, "~") ? d : root ? "/root" : "/home");
    }
    if (path_under(svc->exec, dirs, n)) what = "exec";
    else if (path_under(svc->ready_path, dirs, n)) what = "ready_path";
    else if (wd[0] && path_under(wd, dirs, n)) {
        /* explicit knob stays: asked for by name; the spawn's chdir then fails loudly */
        if (!(svc->hard_default & bit)) return 0;
        what = "working_dir";
    }
    if (!what) return 0;
    if (svc->hard_default & bit) {
        *knob = 0;
        svc->hard_default &= ~bit;
        svc->hard_dropped |= bit;
        fprintf(stderr, "[schema-init] %s: default %s would hide %s — dropped\n",
                svc->name, knob_name, what);
        return 0;
    }
    fprintf(stderr, "[schema-init] %s: %s hides %s — not loaded\n",
            svc->name, knob_name, what);
    return -1;
}

int service_landlock_on(const service_t *svc) {
    return svc->landlock_count || svc->landlock_tcp_n[0] >= 0 || svc->landlock_tcp_n[1] >= 0;
}

static int landlock_check(service_t *svc) {
    int i;
    if (!service_landlock_on(svc)) return 0;
    if (svc->landlock_count) {
        for (i = 0; i < svc->landlock_count; i++)
            if (landlock_beneath(svc->exec, svc->landlock[i])) break;
        if (i == svc->landlock_count) {
            fprintf(stderr, "[schema-init] %s: landlock does not cover exec — not loaded\n",
                    svc->name);
            return -1;
        }
        for (int p = 0; p < svc->exec_pre_count; p++) {
            const char *c = svc->exec_pre[p];
            size_t pl = strspn(c, "+-");
            char *w[32];
            int nw, covered = 0;
            if (memchr(c, '+', pl) || memchr(c, '-', pl)) continue;
            nw = service_split_cmdline(c + pl, w, 32);
            for (i = 0; nw > 0 && i < svc->landlock_count && !covered; i++) {
                char a[PATH_MAX], b[PATH_MAX];
                covered = landlock_beneath(w[0], svc->landlock[i]) ||
                          (realpath(w[0], a) && realpath(svc->landlock[i], b) && landlock_beneath(a, b));
            }
            for (i = 0; i < nw; i++) free(w[i]);
            if (!covered) {
                fprintf(stderr, "[schema-init] %s: landlock does not cover exec_pre=%s — not loaded\n",
                        svc->name, c);
                return -1;
            }
        }
    }
    if (!(svc->flags & SVC_NO_NEW_PRIVS) && svc->cap_restrict &&
        !(svc->cap_keep_mask & (1ULL << CAP_SYS_ADMIN))) {
        fprintf(stderr, "[schema-init] %s: landlock needs no_new_privs=1 when keep_caps "
                "drops CAP_SYS_ADMIN — not loaded\n", svc->name);
        return -1;
    }
    return 0;
}

/* explicit-only knobs: hiding exec/ready_path refuses the load, and an
 * ambient cap the knob strips from the bounding set could never be raised */
static int kernel_ns_check(service_t *svc) {
    static const char *const mods[] = { "/usr/lib/modules/", "/lib/modules/" };
    static const char *const dev[] = { "/dev/" };
    static const char *const kept[] = { "/dev/shm/", "/dev/mqueue/" };
    uint8_t m = !!(svc->ns_kernel & NS_K_MODULES), d = !!(svc->ns_kernel & NS_K_DEVICES);
    uint64_t lost = svc->cap_ambient_mask & ns_kernel_cap_drop(svc->ns_kernel);
    if (ns_hides(svc, 0, &m, "protect_kernel_modules", mods, 2) != 0) return -1;
    /* shm and mqueue are the host's inside the private /dev */
    if (d && !path_under(svc->exec, dev, 1) && path_under(svc->ready_path, kept, 2)) d = 0;
    if (ns_hides(svc, 0, &d, "private_devices", dev, 1) != 0) return -1;
    if (lost) {
        fprintf(stderr, "[schema-init] %s: ambient_caps holds a cap %s drops — not loaded\n",
                svc->name, (lost & (1ULL << CAP_SYS_MODULE)) ? "protect_kernel_modules" : "private_devices");
        return -1;
    }
    return 0;
}

static int sched_finalize(service_t *svc) {
    int rt = svc->sched_policy == SCHED_FIFO || svc->sched_policy == SCHED_RR;
    if (svc->sched_prio && !rt) {
        fprintf(stderr, "[schema-init] %s: sched_priority needs sched_policy=fifo or rr — not loaded\n",
                svc->name);
        return -1;
    }
    if (rt && !svc->sched_prio) svc->sched_prio = 1;
    return 0;
}

static int hardening_finalize(service_t *svc) {
    static const char *const tmp[] = { "/tmp/", "/var/tmp/" };
    static const char *const home[] = { "/home/", "/root/", "/run/user/" };
    apply_hardening_defaults(svc);
    apply_dynuser_implies(svc);
    if (ns_hides(svc, HARD_PT, &svc->ns_private_tmp, "private_tmp", tmp, 2) != 0) return -1;
    if (ns_hides(svc, HARD_PH, &svc->ns_protect_home, "protect_home", home, 3) != 0) return -1;
    if (kernel_ns_check(svc) != 0) return -1;
    if (sched_finalize(svc) != 0) return -1;
    return landlock_check(svc);
}

int service_apply_hardening(const service_t *svc) {
    const char *step = "";
    if ((svc->ns_kernel & NS_K_NETWORK) && private_net() != 0) {
        dprintf(2, "[schema-init] HARDENING FAILED for %s: private_network: %d\n", svc->name, errno);
        return -1;
    }
    if (apply_mount_ns(svc->name, svc->ns_private_tmp, svc->ns_protect_system,
                       svc->ns_protect_home, svc->ns_kernel & NS_K_MOUNT_BITS,
                       (const char *const *)svc->rw_path,
                       svc->rw_path_count, &step) != 0) {
        dprintf(2, "[schema-init] HARDENING FAILED for %s: mount_ns: %s: %d\n",
                svc->name, step, errno);
        return -1;
    }
    if (svc->dynamic_user && geteuid() == 0 && service_private_binds(svc) != 0) {
        dprintf(2, "[schema-init] HARDENING FAILED for %s: dynamic_user private dirs: %d\n", svc->name, errno);
        return -1;
    }
    uint64_t kdrop = ns_kernel_cap_drop(svc->ns_kernel);
    if (svc->cap_restrict) {
        /* an ambient cap outside the bounding set could never be raised;
         * the kernel knobs win over keep_caps, as in systemd */
        if (apply_capabilities((svc->cap_keep_mask | svc->cap_ambient_mask) & ~kdrop) != 0) {
            dprintf(2, "[schema-init] HARDENING FAILED for %s: capabilities: %d\n",
                    svc->name, errno);
            return -1;
        }
    } else if (kdrop && drop_bounding(kdrop) != 0) {
        dprintf(2, "[schema-init] HARDENING FAILED for %s: kernel caps: %d\n", svc->name, errno);
        return -1;
    }
    if (svc->flags & SVC_NO_NEW_PRIVS) {
        if (apply_no_new_privs() != 0) {
            dprintf(2, "[schema-init] HARDENING FAILED for %s: no_new_privs: %d\n",
                    svc->name, errno);
            return -1;
        }
    }
    /* last, and never skipped: the service asked for it, so a kernel that
     * can't (before 6.3) fails the start rather than run it unprotected */
    if ((svc->ns_kernel & NS_K_MDWE) && apply_mdwe() != 0) {
        dprintf(2, "[schema-init] HARDENING FAILED for %s: memory_deny_write_execute: %s\n",
                svc->name, errno == EINVAL ? "kernel has no PR_SET_MDWE (needs Linux 6.3+)" : strerror(errno));
        return -1;
    }
    return 0;
}

static int env_name_ok(const char *s, size_t n) {
    if (!n || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (size_t i = 1; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    return 1;
}

int service_env_file_read(const char *path, char **pairs, int max) {
    int fd = service_open_trusted(path);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "r");
    if (!f) {
        if (fd >= 0) close(fd);
        return -1;
    }
    char *buf = malloc(65537);
    size_t len = buf ? fread(buf, 1, 65536, f) : 0;
    if (len == 65536 && fgetc(f) != EOF)
        fprintf(stderr, "[schema-init] env_file %s: only the first 64 KiB read\n", path);
    fclose(f);
    if (!buf) return 0;
    buf[len] = '\0';
    char *p = buf;
    int n = 0;
    while (*p && n < max) {
        p += strspn(p, " \t\r\n");
        if (!*p) break;
        if (*p == '#' || *p == ';') { p += strcspn(p, "\n"); continue; }
        if (!strncmp(p, "export ", 7)) p += 7;
        char *k = p;
        p += strcspn(p, "=\n");
        if (*p != '=') continue;
        char *ke = p;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        p++;
        p += strspn(p, " \t");
        size_t o = ke - k, keep;
        char *out = malloc(o + strlen(p) + 2);
        if (!out) break;
        memcpy(out, k, o);
        out[o++] = '=';
        keep = o;
        while (*p && *p != '\n') {
            if (*p == '\'') {
                for (p++; *p && *p != '\''; ) out[o++] = *p++;
                if (*p) p++;
                keep = o;
            } else if (*p == '"') {
                for (p++; *p && *p != '"'; ) {
                    if (*p == '\\' && p[1] == '\n') { p += 2; continue; }
                    if (*p == '\\' && p[1] && strchr("\"\\`$", p[1])) p++;
                    out[o++] = *p++;
                }
                if (*p) p++;
                keep = o;
            } else if (*p == '\\' && p[1]) {
                if (p[1] == '\n') { p += 2; continue; }
                p++;
                out[o++] = *p++;
                keep = o;
            } else {
                if (*p != ' ' && *p != '\t' && *p != '\r') keep = o + 1;
                out[o++] = *p++;
            }
        }
        out[keep] = '\0';
        if (env_name_ok(k, ke - k)) pairs[n++] = out;
        else free(out);
    }
    free(buf);
    return n;
}

static int split_words(const char *s, char **out, int max, int relax);

int service_expand_argv(char *const *argv, char **out, int max) {
    int n = 0;
    for (int i = 0; argv[i]; i++) {
        const char *a = argv[i];
        if (a[0] == '$' && env_name_ok(a + 1, strlen(a + 1))) {
            const char *v = getenv(a + 1);
            int k = split_words(v ? v : "", out + n, max - n, 1);
            if (k < 0) return -1;
            n += k;
            continue;
        }
        if (n >= max) return -1;
        char *res = NULL;
        size_t rlen = 0;
        FILE *m = open_memstream(&res, &rlen);
        if (!m) { out[n++] = (char *)a; continue; }
        for (const char *p = a; *p; p++) {
            if (p[0] == '$' && p[1] == '$') { fputc('$', m); p++; continue; }
            if (p[0] == '$' && p[1] == '{') {
                const char *e = strchr(p + 2, '}');
                if (e && env_name_ok(p + 2, e - p - 2)) {
                    char name[256];
                    snprintf(name, sizeof name, "%.*s", (int)(e - p - 2), p + 2);
                    const char *v = getenv(name);
                    if (v) fputs(v, m);
                    p = e;
                    continue;
                }
            }
            fputc(*p, m);
        }
        fclose(m);
        out[n++] = res;
    }
    return n;
}

static int split_words(const char *s, char **out, int max, int relax) {
    int n = 0;
    size_t len = strlen(s);
    while (n < max) {
        s += strspn(s, " \t\n\r");
        if (!*s) break;
        char *w = malloc(len + 1), *o = w;
        if (!w) break;
        while (*s && !strchr(" \t\n\r", *s)) {
            if (*s == '\'') {
                for (s++; *s && *s != '\''; ) *o++ = *s++;
                if (!*s && !relax) { free(w); goto bad; }
                if (*s) s++;
            } else if (*s == '"') {
                for (s++; *s && *s != '"'; ) {
                    if (*s == '\\' && s[1]) s++;
                    *o++ = *s++;
                }
                if (!*s && !relax) { free(w); goto bad; }
                if (*s) s++;
            } else if (*s == '\\' && s[1]) {
                s++;
                *o++ = *s++;
            } else
                *o++ = *s++;
        }
        *o = '\0';
        out[n++] = w;
    }
    if (*(s + strspn(s, " \t\n\r"))) goto bad;
    return n;
bad:
    while (n) free(out[--n]);
    return -1;
}

int service_split_cmdline(const char *s, char **out, int max) {
    return split_words(s, out, max, 0);
}

static void svc_apply_env(const service_t *svc, char **file_env, int file_envc) {
    for (int e = 0; e < svc->env_count; e++) {
        char *kv = svc->envp[e];
        char *ev = strchr(kv, '=');
        if (!ev) continue;
        *ev = '\0';
        setenv(kv, ev + 1, 1);
        *ev = '=';
    }
    for (int e = 0; e < file_envc; e++) {
        char *ev = strchr(file_env[e], '=');
        *ev = '\0';
        setenv(file_env[e], ev + 1, 1);
        *ev = '=';
    }
}

/* Run CMDS in order in this (child) process; PRIVILEGED -1 runs every one,
 * else only those whose '+' prefix matches. A failure without '-' stops the
 * list and returns -1. */
static void svc_seccomp_or_die(const service_t *svc, const char *what);

static int svc_run_cmds(const service_t *svc, char *const *cmds, int n,
                         const char *what, int privileged) {
    for (int i = 0; i < n; i++) {
        const char *c = cmds[i];
        size_t pl = strspn(c, "+-");
        int plus = memchr(c, '+', pl) != NULL, ignore = memchr(c, '-', pl) != NULL;
        if (privileged >= 0 && plus != privileged) continue;
        char *w[33], *x[33], **argv = x;
        int nw = service_split_cmdline(c + pl, w, 32);
        if (nw <= 0) return -1;
        w[nw] = NULL;
        if (svc->expand_args) {
            int nx = service_expand_argv(w, x, 32);
            if (nx < 0) {
                dprintf(2, "[schema-init] %s: %s %s expands to more than 32 words%s\n",
                        svc->name, what, c, ignore ? ", ignored" : "");
                if (!ignore) return -1;
                continue;
            }
            x[nx] = NULL;
        } else {
            memcpy(x, w, nw * sizeof *w);
            x[nw] = NULL;
        }
        pid_t p = fork();
        if (p == 0) {
            /* each command meets the service's filter, not the runner that
             * waits for it; + lines run unsandboxed, as in systemd */
            if (!plus) svc_seccomp_or_die(svc, "");
            execv(argv[0], argv);
            _exit(127);
        }
        int st = 0;
        while (p > 0 && waitpid(p, &st, 0) < 0 && errno == EINTR) ;
        if (p < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            dprintf(2, "[schema-init] %s: %s %s failed (%s %d)%s\n", svc->name, what, c,
                    p < 0 ? "fork" : WIFEXITED(st) ? "exit" : "signal",
                    p < 0 ? errno : WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st),
                    ignore ? ", ignored" : "");
            if (!ignore) return -1;
        }
    }
    return 0;
}

enum { WD_ROOT_PRE, WD_MAIN, WD_STOP };

/* WD_ROOT_PRE runs as root before hardening, for the '+' exec_pre lines only,
 * and is never fatal; WD_MAIN after hardening + the uid drop is the real one.
 * Any failure that doesn't abort lands in "/" — a cwd left over from the root
 * pass would reach into a dir protect_home/private_tmp just hid. WD_STOP never
 * aborts: exec_stop_post must still run. */
static void svc_chdir(const service_t *svc, int pass) {
    if (!svc->working_dir[0]) return;
    if (pass == WD_ROOT_PRE) {
        int plus = 0;
        for (int i = 0; i < svc->exec_pre_count; i++)
            plus |= memchr(svc->exec_pre[i], '+', strspn(svc->exec_pre[i], "+-")) != NULL;
        if (!plus) return;
    }
    const char *d = svc->working_dir;
    int ignore = *d == '-';
    d += ignore;
    if (strcmp(d, "~") == 0) {
        /* by name: run_uid is still 0 in the parent when user= was created after load */
        struct passwd *pw = svc->run_user[0] ? getpwnam(svc->run_user) : getpwuid(svc->run_uid);
        d = pw && pw->pw_dir && pw->pw_dir[0] ? pw->pw_dir : "/";
    }
    if (chdir(d) == 0) return;
    int e = errno;
    if (pass == WD_MAIN && !ignore) {
        dprintf(2, "[schema-init] WORKING DIR FAILED for %s: %s: %d\n", svc->name, d, e);
        _exit(126);
    }
    if (pass == WD_STOP && !ignore)
        dprintf(2, "[schema-init] %s: stop commands: working_dir %s: %d, running in /\n", svc->name, d, e);
    if (chdir("/") != 0) _exit(126);
}

static void svc_run_pre(const service_t *svc, int privileged) {
    if (svc_run_cmds(svc, svc->exec_pre, svc->exec_pre_count, "exec_pre", privileged) < 0) _exit(1);
}

int service_listen_parse(const char *spec, int *type, struct sockaddr_storage *ss,
                         socklen_t *len) {
    static const struct { const char *k; int t; } kinds[] = {
        { "stream:", SOCK_STREAM }, { "dgram:", SOCK_DGRAM },
        { "seqpacket:", SOCK_SEQPACKET }, { "fifo:", 0 },
    };
    const char *a = NULL;
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (!strncmp(spec, kinds[i].k, strlen(kinds[i].k))) {
            *type = kinds[i].t;
            a = spec + strlen(kinds[i].k);
        }
    if (!a || !*a) return -1;
    memset(ss, 0, sizeof *ss);
    if (*a == '/' || *a == '@') {
        struct sockaddr_un *un = (struct sockaddr_un *)ss;
        size_t n = strlen(a);
        if (n >= sizeof un->sun_path || (*a == '@' && (*type == 0 || n < 2))) return -1;
        un->sun_family = AF_UNIX;
        memcpy(un->sun_path, a, n);
        if (*a == '@') {
            un->sun_path[0] = '\0';
            *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n);
        } else
            *len = sizeof *un;
        return 0;
    }
    if (*type == 0 || *type == SOCK_SEQPACKET) return -1;
    char host[64] = "";
    const char *port = a, *c;
    if (*a == '[') {
        c = strchr(a, ']');
        if (!c || c[1] != ':' || (size_t)(c - a - 1) >= sizeof host) return -1;
        memcpy(host, a + 1, c - a - 1);
        port = c + 2;
    } else if ((c = strrchr(a, ':'))) {
        if ((size_t)(c - a) >= sizeof host) return -1;
        memcpy(host, a, c - a);
        port = c + 1;
    }
    char *end;
    long p = strtol(port, &end, 10);
    if (!*port || *end || p < 1 || p > 65535) return -1;
    struct sockaddr_in *in4 = (struct sockaddr_in *)ss;
    struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ss;
    if (*a != '[' && host[0] && inet_pton(AF_INET, host, &in4->sin_addr) == 1) {
        in4->sin_family = AF_INET;
        in4->sin_port = htons((uint16_t)p);
        *len = sizeof *in4;
        return 0;
    }
    if (host[0] && inet_pton(AF_INET6, host, &in6->sin6_addr) != 1) return -1;
    if (!host[0]) in6->sin6_addr = in6addr_any;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons((uint16_t)p);
    *len = sizeof *in6;
    return 0;
}

/* The socket's directory as a dirfd, created 0755 where missing. Every
 * directory on the way must be ours (root's) and writable by no one else, or
 * sticky: PID 1 unlinks, creates and chowns the entry, so nobody may swap a
 * component under it. */
static int listen_dir(const char *path, const char **base) {
    char d[128];
    int dfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    snprintf(d, sizeof d, "%s", path);
    char *p = d + 1, *slash;
    while (dfd >= 0 && (slash = strchr(p, '/'))) {
        struct stat st;
        *slash = '\0';
        mkdirat(dfd, p, 0755);
        int n = openat(dfd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dfd);
        dfd = n;
        if (dfd >= 0 && (fstat(dfd, &st) < 0 || !svc_dir_trusted(&st, 1))) {
            close(dfd);
            dfd = -1;
            errno = EPERM;
        }
        p = slash + 1;
    }
    *base = path + (p - d);
    return dfd;
}

static int listen_one(const service_t *svc, const char *spec) {
    struct sockaddr_storage ss;
    socklen_t len;
    int type, fd, one = 1, zero = 0, dfd = -1;
    mode_t mode = svc->socket_mode ? (mode_t)svc->socket_mode : 0666;
    if (service_listen_parse(spec, &type, &ss, &len) < 0) {
        errno = EINVAL;
        return -1;
    }
    const char *path = ss.ss_family == AF_UNIX && ((struct sockaddr_un *)&ss)->sun_path[0]
                     ? ((struct sockaddr_un *)&ss)->sun_path : NULL, *base = NULL;
    uid_t u = (uid_t)-1;
    gid_t g = (gid_t)-1;
    if (path) {
        struct passwd *pw = svc->socket_user[0] ? getpwnam(svc->socket_user) : NULL;
        struct group *gr = svc->socket_group[0] ? getgrnam(svc->socket_group) : NULL;
        if (pw) u = pw->pw_uid;
        if (gr) g = gr->gr_gid;
        if ((svc->socket_user[0] && !pw) || (svc->socket_group[0] && !gr))
            fprintf(stderr, "[schema-init] %s: socket owner %s:%s not found, left as root\n",
                    svc->name, svc->socket_user, svc->socket_group);
        mode_t old = umask(022);
        dfd = listen_dir(path, &base);
        umask(old);
        if (dfd < 0) return -1;
    }
    struct stat st;
    if (type == 0) {
        mode_t old = umask(0777 & ~mode);
        int r = mkfifoat(dfd, base, mode);
        umask(old);
        fd = r < 0 && errno != EEXIST ? -1
           : openat(dfd, base, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0 && (fstat(fd, &st) < 0 || !S_ISFIFO(st.st_mode))) {
            close(fd);
            fd = -1;
            errno = EEXIST;
        }
        if (fd >= 0) {
            fchmod(fd, mode);
            if (u != (uid_t)-1 || g != (gid_t)-1) fchown(fd, u, g);
        }
        close(dfd);
        return fd;
    }
    if (path && fstatat(dfd, base, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISSOCK(st.st_mode))
        unlinkat(dfd, base, 0);
    fd = socket(ss.ss_family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        if (ss.ss_family != AF_UNIX)
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (ss.ss_family == AF_INET6)
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        mode_t old = umask(0777 & ~mode);
        int r = bind(fd, (struct sockaddr *)&ss, len);
        umask(old);
        if (r < 0 || (type != SOCK_DGRAM && listen(fd, SOMAXCONN) < 0)) {
            int e = errno;
            close(fd);
            fd = -1;
            errno = e;
        }
    }
    if (fd >= 0 && path && (u != (uid_t)-1 || g != (gid_t)-1))
        fchownat(dfd, base, u, g, AT_SYMLINK_NOFOLLOW);
    if (dfd >= 0) {
        int e = errno;
        close(dfd);
        errno = e;
    }
    return fd;
}

int service_listen_open(service_t *svc) {
    if (svc->listen_open) return 0;
    for (int i = 0; i < svc->listen_count; i++) {
        int fd = listen_one(svc, svc->listen[i]);
        if (fd < 0) {
            fprintf(stderr, "[schema-init] %s: listen=%s: %s\n", svc->name, svc->listen[i], strerror(errno));
            while (i) close(svc->listen_fd[--i]);
            return -1;
        }
        svc->listen_fd[i] = fd;
    }
    svc->listen_open = svc->listen_count;
    return 0;
}

static const char *const cond_kinds[] = {
    "path_exists", "path_exists_glob", "path_is_dir", "path_is_symlink", "path_is_mount",
    "path_is_rw", "dir_not_empty", "file_not_empty", "file_is_exec", "ac_power",
    "kernel_cmdline", NULL,
};

static int cond_kind(const char *c, const char **arg) {
    const char *colon = strchr(c, ':');
    if (!colon) return -1;
    for (int i = 0; cond_kinds[i]; i++)
        if ((size_t)(colon - c) == strlen(cond_kinds[i]) && !strncmp(c, cond_kinds[i], colon - c)) {
            *arg = colon + 1;
            return i;
        }
    return -1;
}

/* systemd's rule: on AC if an adapter is online, or if there is no system
 * adapter at all (a desktop); device-scope supplies (a mouse) don't count */
static int on_ac_power(void) {
    DIR *d = opendir("/sys/class/power_supply");
    struct dirent *e;
    int online = 0, offline = 0;
    if (!d) return 1;
    while ((e = readdir(d))) {
        char p[300], v[32] = "";
        FILE *f;
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/scope", e->d_name);
        if ((f = fopen(p, "r"))) {
            int dev = fgets(v, sizeof v, f) && !strncmp(v, "Device", 6);
            fclose(f);
            if (dev) continue;
        }
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        if (!(f = fopen(p, "r"))) continue;
        if (!fgets(v, sizeof v, f)) v[0] = '\0';
        fclose(f);
        if (strncmp(v, "Mains", 5) && strncmp(v, "USB", 3)) continue;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name);
        if ((f = fopen(p, "r"))) {
            if (fgets(v, sizeof v, f) && v[0] == '1') online = 1;
            else offline = 1;
            fclose(f);
        }
    }
    closedir(d);
    return online || !offline;
}

/* one condition= line, with its own '!' applied; '|' is the caller's */
int service_condition_check(const char *c) {
    const char *a;
    int k = cond_kind(c, &a), neg, r = 0;
    struct stat st;
    if (k < 0) return -1;
    a += *a == '|';
    neg = *a == '!';
    a += neg;
    switch (k) {
    case 0: r = access(a, F_OK) == 0; break;
    case 1: {
        glob_t g;
        r = glob(a, GLOB_NOSORT, NULL, &g) == 0;
        if (r) globfree(&g);
        break;
    }
    case 2: r = stat(a, &st) == 0 && S_ISDIR(st.st_mode); break;
    case 3: r = lstat(a, &st) == 0 && S_ISLNK(st.st_mode); break;
    case 4: {
        struct statx sx;
        r = statx(AT_FDCWD, a, 0, STATX_BASIC_STATS, &sx) == 0 &&
            (sx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) &&
            (sx.stx_attributes & STATX_ATTR_MOUNT_ROOT);
        break;
    }
    case 5: {
        struct statvfs sv;
        r = statvfs(a, &sv) == 0 && !(sv.f_flag & ST_RDONLY);
        break;
    }
    case 6: {
        DIR *d = opendir(a);
        struct dirent *e;
        if (d) {
            while (!r && (e = readdir(d)))
                r = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
            closedir(d);
        }
        break;
    }
    case 7: r = stat(a, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0; break;
    case 8: r = stat(a, &st) == 0 && S_ISREG(st.st_mode) && access(a, X_OK) == 0; break;
    case 9: {
        int want = !strcmp(a, "true") || !strcmp(a, "yes") || !strcmp(a, "1");
        r = on_ac_power() == want;
        break;
    }
    case 10: {
        char cl[4096] = "";
        int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = read(fd, cl, sizeof cl - 1);
            if (n > 0) cl[n] = '\0';
            close(fd);
        }
        r = cmdline_word_match(cl, a);
        break;
    }
    }
    return neg ? !r : r;
}

int service_conditions_met(const service_t *svc, char *why, size_t n) {
    int triggers = 0, any = 0;
    for (int i = 0; i < svc->cond_count; i++) {
        const char *a;
        cond_kind(svc->cond[i], &a);
        int trig = *a == '|', r = service_condition_check(svc->cond[i]) == 1;
        if (trig) {
            triggers++;
            any |= r;
        } else if (!r) {
            snprintf(why, n, "%s", svc->cond[i]);
            return 0;
        }
    }
    if (triggers && !any) {
        snprintf(why, n, "none of the %d |-conditions", triggers);
        return 0;
    }
    return 1;
}

int service_listen_matches(const service_t *svc, int k) {
    struct sockaddr_storage want, got;
    socklen_t wl, gl = sizeof got;
    int type, gt;
    socklen_t tl = sizeof gt;
    struct stat a, b;
    if (service_listen_parse(svc->listen[k], &type, &want, &wl) < 0 ||
        fstat(svc->listen_fd[k], &a) < 0)
        return 0;
    if (type == 0)
        return S_ISFIFO(a.st_mode) && stat(((struct sockaddr_un *)&want)->sun_path, &b) == 0 &&
               a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    if (!S_ISSOCK(a.st_mode) ||
        getsockopt(svc->listen_fd[k], SOL_SOCKET, SO_TYPE, &gt, &tl) < 0 || gt != type ||
        getsockname(svc->listen_fd[k], (struct sockaddr *)&got, &gl) < 0 ||
        got.ss_family != want.ss_family)
        return 0;
    if (want.ss_family == AF_UNIX) {
        const char *w = ((struct sockaddr_un *)&want)->sun_path, *g = ((struct sockaddr_un *)&got)->sun_path;
        return w[0] ? g[0] && !strncmp(w, g, sizeof ((struct sockaddr_un *)0)->sun_path)
                    : gl == wl && !memcmp(w, g, wl - offsetof(struct sockaddr_un, sun_path));
    }
    if (want.ss_family == AF_INET)
        return ((struct sockaddr_in *)&got)->sin_port == ((struct sockaddr_in *)&want)->sin_port &&
               ((struct sockaddr_in *)&got)->sin_addr.s_addr == ((struct sockaddr_in *)&want)->sin_addr.s_addr;
    return ((struct sockaddr_in6 *)&got)->sin6_port == ((struct sockaddr_in6 *)&want)->sin6_port &&
           !memcmp(&((struct sockaddr_in6 *)&got)->sin6_addr, &((struct sockaddr_in6 *)&want)->sin6_addr,
                   sizeof(struct in6_addr));
}

void service_listen_close(service_t *svc) {
    for (int i = 0; i < svc->listen_open; i++) close(svc->listen_fd[i]);
    svc->listen_open = 0;
}

/* systemd gives every run a fresh 128-bit INVOCATION_ID; daemons read it as
 * "a service manager runs me" (irqbalance stays in the foreground) */
static void svc_set_invocation_id(void) {
    unsigned char b[16];
    char hex[33];
    if (getrandom(b, sizeof b, GRND_NONBLOCK) != sizeof b) {
        unsetenv("INVOCATION_ID");
        return;
    }
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", b[i]);
    setenv("INVOCATION_ID", hex, 1);
}

/* sd_listen_fds(3): the sockets at fd 3.., LISTEN_PID our own pid */
static int svc_pass_listen(const service_t *svc) {
    int lo = svc->listen_open, stored = fdstore_count(svc->name);
    int n = lo + stored;
    if (!n) {
        unsetenv("LISTEN_FDS");
        unsetenv("LISTEN_PID");
        unsetenv("LISTEN_FDNAMES");
        return 0;
    }
    int *src = malloc((size_t)n * sizeof *src);
    const char **sn = malloc((size_t)(stored ? stored : 1) * sizeof *sn);
    if (!src || !sn) _exit(126);
    for (int i = 0; i < lo; i++) src[i] = svc->listen_fd[i];
    stored = fdstore_collect(svc->name, src + lo, sn, stored);
    n = lo + stored;
    if (fdstore_place_fds(src, n) < 0) _exit(126);
    size_t cap = (size_t)n * 72 + 1;
    char *names = malloc(cap);
    if (!names || fdstore_fdnames(svc->name, lo, names, cap) < 0) _exit(126);
    char v[24];
    snprintf(v, sizeof v, "%d", n);
    setenv("LISTEN_FDS", v, 1);
    snprintf(v, sizeof v, "%d", (int)getpid());
    setenv("LISTEN_PID", v, 1);
    setenv("LISTEN_FDNAMES", names, 1);
    return stored;
}

/* close_range fallback: no allocation, we are in the forked child */
static void close_fds_from(int lo) {
    char buf[4096];
    int closed = 1;
    while (closed) {
        int dfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        long n;
        closed = 0;
        if (dfd < 0) {
            for (int fd = lo; fd < 65536; fd++) close(fd);
            return;
        }
        while ((n = syscall(SYS_getdents64, dfd, buf, sizeof buf)) > 0) {
            for (long off = 0; off < n; ) {
                unsigned short reclen = *(unsigned short *)(buf + off + 16);
                const char *name = buf + off + 19;
                char *end;
                long fd = strtol(name, &end, 10);
                if (*name != '.' && !*end && fd >= lo && fd != dfd && close((int)fd) == 0)
                    closed = 1;
                off += reclen;
            }
        }
        close(dfd);
    }
}

static void svc_child_log(const service_t *svc) {
    char log_path[320];
    int fd;
    if (service_log_dir) {
        mkdir(service_log_dir, 0700);
        snprintf(log_path, sizeof(log_path), "%s/%s.log", service_log_dir, svc->name);
        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
    } else {
        mkdir("/var/log/schema-init", 0755);
        snprintf(log_path, sizeof(log_path), "/var/log/schema-init/%s.log", svc->name);
        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
    }
    if (fd < 0 && !service_log_dir) {
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
}

service_t *service_fallback(const service_t *svc, service_t *table, int count) {
    for (int c = 0; c < svc->card_count; c++) {
        if (strncmp(svc->card[c], "fallback:", 9) != 0) continue;
        for (int j = 0; j < count; j++)
            if (&table[j] != svc && strcmp(table[j].name, svc->card[c] + 9) == 0) return &table[j];
    }
    return NULL;
}

int service_is_standby(const service_t *svc, const service_t *table, int count) {
    for (int i = 0; i < count; i++)
        for (int c = 0; &table[i] != svc && c < table[i].card_count; c++)
            if (strncmp(table[i].card[c], "fallback:", 9) == 0 && strcmp(table[i].card[c] + 9, svc->name) == 0)
                return 1;
    return 0;
}

/* group= as a name or numeric gid; 1 and *out set when it resolves */
static int svc_group_resolve(const char *val, gid_t *out) {
    char *end;
    errno = 0;
    unsigned long id = strtoul(val, &end, 10);
    int num = *val && !*end;
    struct group *gr = !num ? getgrnam(val)
                     : !errno && id < (gid_t)-1 ? getgrgid((gid_t)id) : NULL;
    if (!gr) return 0;
    if (out) *out = gr->gr_gid;
    return 1;
}

/* run_group -> *g in a spawned child; exits 126 on failure: a service asked
 * to run under a group must never fall back to root:root. *g is untouched
 * when no group= is set. */
static void svc_resolve_group_or_die(const service_t *svc, gid_t *g, const char *what) {
    if (!svc->run_group[0]) return;
    if (!svc_group_resolve(svc->run_group, g)) {
        dprintf(2, "[schema-init] UID DROP FAILED for %s: %sno group %s\n",
                svc->name, what, svc->run_group);
        _exit(126);
    }
}

/* In a just-forked runner (exec_stop/exec_stop_post, exec_reload): own
 * process group, so a timed-out runner is killed with what it started; the
 * service's env, user and working dir. WHAT names it in failure lines. */
/* NULL, or the key that couldn't be applied (errno set). nice is a hint,
 * as in systemd: refused, the closest RLIMIT_NICE allows, with a log line */
static const char *svc_apply_sched(const service_t *svc) {
    if (svc->nice_set && setpriority(PRIO_PROCESS, 0, svc->nice_val) != 0) {
        struct rlimit rl;
        int floor = getpriority(PRIO_PROCESS, 0);
        if (getrlimit(RLIMIT_NICE, &rl) == 0 && rl.rlim_cur > 0 && 20 - (int)rl.rlim_cur < floor)
            floor = 20 - (int)rl.rlim_cur;
        if (floor <= svc->nice_val || setpriority(PRIO_PROCESS, 0, floor) != 0) return "nice";
        dprintf(2, "[schema-init] %s: nice=%d not allowed, running at %d\n", svc->name, svc->nice_val, floor);
    }
    if (svc->io_class >= 0 || svc->io_prio >= 0) {
        int cls = svc->io_class >= 0 ? svc->io_class : 2;
        int data = cls == 1 || cls == 2 ? (svc->io_prio >= 0 ? svc->io_prio : 4) : 0;
        /* IOPRIO_PRIO_VALUE(class, data), IOPRIO_WHO_PROCESS */
        if (syscall(SYS_ioprio_set, 1, 0, cls << 13 | data) != 0) return "io_class";
    }
    if (svc->sched_policy >= 0) {
        struct sched_param sp = { .sched_priority = svc->sched_prio };
        if (sched_setscheduler(0, svc->sched_policy, &sp) != 0) return "sched_policy";
    }
    return NULL;
}

static void svc_sched_or_die(const service_t *svc) {
    const char *what = svc_apply_sched(svc);
    if (what) {
        dprintf(2, "[schema-init] SCHEDULING FAILED for %s: %s: %s\n", svc->name, what, strerror(errno));
        _exit(126);
    }
}

/* exec_stop/exec_reload talk to the service (rndc reload, a curl to its
 * 127.0.0.1 admin port): with private_network they belong in its netns, as
 * under systemd. Main gone: a fresh empty one, never the host's */
static void svc_runner_netns(const service_t *svc, const char *what) {
    char p[40];
    int fd = -1;
    if (!(svc->ns_kernel & NS_K_NETWORK)) return;
    if (svc->child_pid > 0) {
        snprintf(p, sizeof p, "/proc/%d/ns/net", (int)svc->child_pid);
        fd = open(p, O_RDONLY | O_CLOEXEC);
    }
    if (fd >= 0 ? setns(fd, CLONE_NEWNET) != 0 : private_net() != 0) {
        dprintf(2, "[schema-init] HARDENING FAILED for %s: %s: private_network: %s\n",
                svc->name, what, strerror(errno));
        _exit(126);
    }
    if (fd >= 0) close(fd);
}

/* the very last step before execve: the service's own setup never meets
 * its filter, and execve itself is always allowed */
static void svc_seccomp_or_die(const service_t *svc, const char *what) {
    const char *step = "";
    if (sc_active(&svc->sc) && sc_install(&svc->sc, &step) != 0) {
        dprintf(2, "[schema-init] HARDENING FAILED for %s: %sseccomp: %s: %d\n", svc->name, what, step, errno);
        _exit(126);
    }
}

/* keepcaps + inheritable before the uid drop, so ambient caps survive it */
static void svc_ambient_prep_or_die(const service_t *svc) {
    const char *step = "";
    if (svc->cap_ambient_mask && ambient_caps_prep(svc->cap_ambient_mask, &step) != 0) {
        dprintf(2, "[schema-init] AMBIENT CAPS FAILED for %s: %s: %d\n", svc->name, step, errno);
        _exit(126);
    }
}

/* the user's /etc/group memberships; a dynamic uid has none, so it starts
 * from an empty list (not nobody's, which initgroups would hand it) */
static int svc_initgroups(const service_t *svc, gid_t gid) {
    if (svc->dyn_uid) return setgroups(0, NULL);
    return initgroups(svc->run_user[0] ? svc->run_user : svc->run_uid ? "nobody" : "root", gid);
}

/* supplementary_groups=, added to what initgroups() just set (a user may
 * already be in many groups, so the list is sized from getgroups) */
static void svc_supp_groups_or_die(const service_t *svc) {
    if (!svc->supp_count) return;
    int n = getgroups(0, NULL);
    gid_t *g = n < 0 ? NULL : malloc(sizeof(gid_t) * (size_t)(n + 16));
    if (!g || (n = getgroups(n, g)) < 0) {
        dprintf(2, "[schema-init] UID DROP FAILED for %s: supplementary_groups: %d\n", svc->name, errno);
        _exit(126);
    }
    for (int i = 0; i < svc->supp_count; i++) {
        gid_t gid;
        if (!svc_group_resolve(svc->supp_groups[i], &gid)) {
            dprintf(2, "[schema-init] UID DROP FAILED for %s: supplementary_groups: %s\n",
                    svc->name, svc->supp_groups[i]);
            _exit(126);
        }
        int dup = 0;
        for (int k = 0; k < n && !dup; k++) dup = g[k] == gid;
        if (!dup) g[n++] = gid;
    }
    if (setgroups((size_t)n, g) != 0) {
        dprintf(2, "[schema-init] UID DROP FAILED for %s: supplementary_groups: setgroups: %d\n", svc->name, errno);
        _exit(126);
    }
    free(g);
}

static void svc_runner_child(const service_t *svc, const char *what) {
    service_reset_child_sigmask();
    setsid();
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        if (null_fd > 0) close(null_fd);
    }
    svc_child_log(svc);
    /* after the log: PID 1's own files keep PID 1's modes */
    if (svc->umask_val >= 0) umask((mode_t)svc->umask_val);
    syscall(SYS_close_range, 3, ~0U, 0);
    svc_runner_netns(svc, what);
    svc_sched_or_die(svc);
    svc_make_exec_dirs(svc, 0);
    svc_apply_env(svc, NULL, 0);
    char gwhat[48];
    snprintf(gwhat, sizeof gwhat, "%s: ", what);
    gid_t sgid = svc->run_gid;
    svc_resolve_group_or_die(svc, &sgid, gwhat);
    /* the runner reaches $STATE_DIRECTORY through the same private binds */
    if (svc->dynamic_user && geteuid() == 0 && service_private_binds(svc) != 0) {
        dprintf(2, "[schema-init] HARDENING FAILED for %s: %sdynamic_user private dirs: %d\n", svc->name, gwhat, errno);
        _exit(126);
    }
    int amb = svc->run_uid && svc->cap_ambient_mask;
    if (amb || svc->run_uid || svc->run_group[0] || svc->supp_count) {
        const char *step = "";
        if (amb && ambient_caps_prep(svc->cap_ambient_mask, &step) != 0) {
            dprintf(2, "[schema-init] AMBIENT CAPS FAILED for %s: %s: %d\n", svc->name, step, errno);
            _exit(126);
        }
        if (svc_initgroups(svc, sgid) != 0) {
            dprintf(2, "[schema-init] UID DROP FAILED for %s: %s: %d\n", svc->name, what, errno);
            _exit(126);
        }
        svc_supp_groups_or_die(svc);
        if (setgid(sgid) != 0 || (svc->run_uid && setuid(svc->run_uid) != 0)) {
            dprintf(2, "[schema-init] UID DROP FAILED for %s: %s: %d\n", svc->name, what, errno);
            _exit(126);
        }
        if (amb && ambient_caps_raise(svc->cap_ambient_mask, &step) != 0) {
            dprintf(2, "[schema-init] AMBIENT CAPS FAILED for %s: %s: %d\n", svc->name, step, errno);
            _exit(126);
        }
    }
    svc_chdir(svc, WD_STOP);
}

pid_t service_stop_spawn(const service_t *svc, int phase) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    svc_runner_child(svc, "stop commands");
    /* a failing exec_stop skips the rest of its own list, never exec_stop_post */
    int bad = (phase & 1) && svc_run_cmds(svc, svc->exec_stop, svc->exec_stop_count, "exec_stop", -1) < 0;
    if ((phase & 2) && svc_run_cmds(svc, svc->exec_stop_post, svc->exec_stop_post_count, "exec_stop_post", -1) < 0)
        bad = 1;
    _exit(bad);
}

/* $MAINPID / ${MAINPID} replaced by PID; a pid (<= 7 digits) is shorter
 * than either, so the copy never outgrows the original */
static char *svc_subst_mainpid(const char *c, const char *pid) {
    char *o = malloc(strlen(c) + 1);
    if (!o) return NULL;
    size_t n = 0, pl = strlen(pid);
    while (*c) {
        size_t skip = strncmp(c, "${MAINPID}", 10) == 0 ? 10 : strncmp(c, "$MAINPID", 8) == 0 ? 8 : 0;
        if (skip && pl <= skip) {
            memcpy(o + n, pid, pl);
            n += pl;
            c += skip;
            continue;
        }
        o[n++] = *c++;
    }
    o[n] = '\0';
    return o;
}

pid_t service_reload_spawn(const service_t *svc, pid_t mainpid) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    svc_runner_child(svc, "exec_reload");
    char pidtxt[24];
    snprintf(pidtxt, sizeof pidtxt, "%d", (int)mainpid);
    setenv("MAINPID", pidtxt, 1);
    char *cmds[MAX_EXEC_PRE];
    for (int i = 0; i < svc->exec_reload_count; i++)
        if (!(cmds[i] = svc_subst_mainpid(svc->exec_reload[i], pidtxt))) _exit(1);
    _exit(svc_run_cmds(svc, cmds, svc->exec_reload_count, "exec_reload", -1) < 0);
}

pid_t service_card_spawn(const service_t *svc, int idx) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    service_reset_child_sigmask();
    setsid();
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        if (null_fd > 0) close(null_fd);
    }
    svc_child_log(svc);
    if (svc->umask_val >= 0) umask((mode_t)svc->umask_val);
    syscall(SYS_close_range, 3, ~0U, 0);
    char *w[33];
    int nw = service_split_cmdline(svc->card[idx] + 5, w, 32);
    if (nw <= 0) _exit(127);
    w[nw] = NULL;
    execv(w[0], w);
    dprintf(2, "[schema-init] %s: card %s: %s\n", svc->name, w[0], strerror(errno));
    _exit(127);
}

static void svc_resolve_user_or_die(service_t *svc) {
    if (svc->run_user[0] && !svc->run_uid && strcmp(svc->run_user, "root") != 0) {
        char *end;
        unsigned long id = strtoul(svc->run_user, &end, 10);
        struct passwd *pw = !*end && id < (uid_t)-1 ? getpwuid((uid_t)id)
                          : *end ? getpwnam(svc->run_user) : NULL;
        if (!pw) {
            dprintf(2, "[schema-init] UID DROP FAILED for %s: no user %s\n",
                    svc->name, svc->run_user);
            _exit(126);
        }
        svc->run_uid = pw->pw_uid;
        svc->run_gid = pw->pw_gid;
    }
    /* after user=: group= overrides the primary gid */
    svc_resolve_group_or_die(svc, &svc->run_gid, "");
}

int service_spawn(service_t *svc) {
    int sync[2];
    pid_t pid;

    /* schema-ctl restart: a fresh runtime dir, as systemd's stop+start gives;
     * a crash respawn keeps it */
    if (svc->rt_made && svc->ctl_killed) {
        service_runtime_dirs_remove(svc);
        svc->rt_made = 0;
    }
    svc->ready_path_verified = 0;
    svc->ready_watched = 0;
    svc->ctl_killed = 0;
    svc->excise_reason = EXC_NONE;   /* the reason belongs to the run that was excised */
    svc->bind_died_at.tv_sec = svc->bind_died_at.tv_nsec = 0;
    svc->ready_stale_ino = 0;
    if (svc->ready_path[0]) {
        struct stat st;
        if (stat(svc->ready_path, &st) == 0 && !S_ISDIR(st.st_mode)) {
            svc->ready_stale_dev = st.st_dev;
            svc->ready_stale_ino = st.st_ino;
            svc->ready_stale_ctime = st.st_ctim;
        }
    }

    if (svc->listen_count && service_listen_open(svc) < 0) return -1;
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
        service_restore_child_nofile();
        coredump_raise_if_ours();
        setsid();
        close(sync[1]);
        read(sync[0], &c, 1);
        close(sync[0]);
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            if (null_fd > 0) close(null_fd);
        }
        svc_child_log(svc);
        /* before exec_pre: every command of the service gets it, as in systemd */
        if (svc->umask_val >= 0) umask((mode_t)svc->umask_val);
        char *file_env[64];
        int file_envc = 0;
        for (int i = 0; i < svc->env_file_count; i++) {
            const char *ef = svc->env_file[i];
            int opt = *ef == '-';
            int got = service_env_file_read(ef + opt, file_env + file_envc, 64 - file_envc);
            if (got < 0) {
                if (errno == EPERM) {
                    dprintf(2, "[schema-init] %s: REFUSED env_file %s: not a regular file owned by root "
                            "and writable only by its owner (chown root:root; chmod go-w)%s\n",
                            svc->name, ef + opt, opt ? " — optional, skipped" : "");
                    if (opt) continue;
                    _exit(1);
                }
                if (opt) continue;
                dprintf(2, "[schema-init] %s: env_file %s: %s\n", svc->name, ef, strerror(errno));
                _exit(1);
            }
            file_envc += got;
            if (file_envc == 64)
                dprintf(2, "[schema-init] %s: env_file %s: 64-variable limit reached, rest ignored\n",
                        svc->name, ef);
        }
        svc_set_invocation_id();
        /* how long this service gets to stop, so a script can pace its own
         * shutdown inside it (unset = PID 1's default grace) */
        if (svc->stop_timeout_sec) {
            char st[12];
            snprintf(st, sizeof st, "%d", svc->stop_timeout_sec);
            setenv("SCHEMA_STOP_TIMEOUT_SEC", st, 1);
        } else {
            unsetenv("SCHEMA_STOP_TIMEOUT_SEC");
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
        /* an explicit nice= replaces the tier's: never set first, then undone */
        if (svc->nice_set) {
        } else if (svc->priority == PRIO_CRITICAL) {
            if (setpriority(PRIO_PROCESS, 0, -10) < 0) {
                fprintf(stderr, "[schema-init] Warning: failed to set critical priority for %s: %s\n", svc->name, strerror(errno));
            }
        } else if (svc->priority == PRIO_PERIPHERAL) {
            if (setpriority(PRIO_PROCESS, 0, 10) < 0) {
                fprintf(stderr, "[schema-init] Warning: failed to set peripheral priority for %s: %s\n", svc->name, strerror(errno));
            }
        }
        if (svc->oom_adj_set) {
            /* Before the uid drop: lowering it needs CAP_SYS_RESOURCE. Every
             * child inherits it, so a session launcher must not carry one. */
            int ofd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
            if (ofd >= 0) {
                dprintf(ofd, "%d\n", svc->oom_score_adj);
                close(ofd);
            }
        }
        for (int i = 0; i < svc->rlim_count; i++) {
            struct rlimit rl = { svc->rlim[i].cur, svc->rlim[i].max };
            if (svc->rlim[i].res == RLIMIT_NOFILE &&
                (rl.rlim_cur == RLIM_INFINITY || rl.rlim_max == RLIM_INFINITY)) {
                FILE *f = fopen("/proc/sys/fs/nr_open", "r");
                unsigned long nr = 1048576;
                if (f) { fscanf(f, "%lu", &nr); fclose(f); }
                if (rl.rlim_cur == RLIM_INFINITY) rl.rlim_cur = nr;
                if (rl.rlim_max == RLIM_INFINITY) rl.rlim_max = nr;
            }
            if (setrlimit(svc->rlim[i].res, &rl) != 0) {
                dprintf(2, "[schema-init] LIMIT FAILED for %s: resource %d: %d\n",
                        svc->name, svc->rlim[i].res, errno);
                _exit(126);
            }
        }
        /* after limit_nice=/limit_rtprio= (they may be what grants it), before
         * the uid drop (RT needs CAP_SYS_NICE): a policy or I/O class not
         * granted fails the start, as in systemd, rather than run unscheduled */
        svc_sched_or_die(svc);
        /* only when there are dirs to own: otherwise after the + exec_pre
         * lines, which may create the user (sysusers) */
        int exec_dirs = 0;
        for (int k = 0; k < EXEC_DIR_KINDS; k++) exec_dirs |= svc->exec_dir_count[k];
        if (exec_dirs) {
            svc_resolve_user_or_die(svc);
            svc_make_exec_dirs(svc, 1);
        }
        if (svc->exec_pre_count) {
            svc_apply_env(svc, file_env, file_envc);
            svc_chdir(svc, WD_ROOT_PRE);
            svc_run_pre(svc, 1);
        }
        /* user lookups may need the network (LDAP, NIS): before private_network */
        int early_user = !exec_dirs && (svc->ns_kernel & NS_K_NETWORK);
        if (early_user) svc_resolve_user_or_die(svc);
        if (service_apply_hardening(svc) != 0)
            _exit(126);
        if (!exec_dirs && !early_user) svc_resolve_user_or_die(svc);
        if (svc->dyn_uid) {
            /* no passwd entry for a dynamic uid (no NSS module): systemd's env */
            setenv("HOME", "/", 1);
            setenv("USER", svc->name, 1);
            setenv("LOGNAME", svc->name, 1);
            svc_ambient_prep_or_die(svc);
        } else if (svc->run_uid) {
            struct passwd *pw = getpwuid(svc->run_uid);
            if (pw) {
                setenv("HOME", pw->pw_dir, 1);
                setenv("USER", pw->pw_name, 1);
                setenv("LOGNAME", pw->pw_name, 1);
                setenv("SHELL", pw->pw_shell, 1);
            }
            if (!svc->ns_protect_home) {
                char xdg[48];
                snprintf(xdg, sizeof(xdg), "/run/user/%u", (unsigned)svc->run_uid);
                mkdir(xdg, 0700);
                /* the user's primary group, as logind would: group= must not
                 * hand the override group ownership of the runtime dir */
                chown(xdg, svc->run_uid, pw ? pw->pw_gid : svc->run_gid);
                setenv("XDG_RUNTIME_DIR", xdg, 1);
            }
            svc_ambient_prep_or_die(svc);
        }
        /* group= alone still switches gid: root with that group, never root:root;
         * supplementary_groups= alone still sets groups on a root service */
        if (svc->run_uid || svc->run_group[0] || svc->supp_count) {
            if (svc_initgroups(svc, svc->run_gid) != 0) {
                dprintf(2, "[schema-init] UID DROP FAILED for %s: initgroups: %d\n",
                        svc->name, errno);
                _exit(126);
            }
            svc_supp_groups_or_die(svc);
            if (setgid(svc->run_gid) != 0) {
                dprintf(2, "[schema-init] UID DROP FAILED for %s: setgid: %d\n",
                        svc->name, errno);
                _exit(126);
            }
        }
        if (service_landlock_on(svc)) {
            const char *step = "";
            if (apply_landlock(svc->landlock, svc->landlock_rw, svc->landlock_count,
                               svc->landlock_tcp_n, svc->landlock_tcp, &step) != 0) {
                dprintf(2, "[schema-init] HARDENING FAILED for %s: landlock: %s: %d\n",
                        svc->name, step, errno);
                _exit(126);
            }
        }
        if (svc->run_uid && setuid(svc->run_uid) != 0) {
            dprintf(2, "[schema-init] UID DROP FAILED for %s: setuid: %d\n",
                    svc->name, errno);
            _exit(126);
        }
        if (svc->run_uid && svc->cap_ambient_mask) {
            const char *step = "";
            if (ambient_caps_raise(svc->cap_ambient_mask, &step) != 0) {
                dprintf(2, "[schema-init] AMBIENT CAPS FAILED for %s: %s: %d\n",
                        svc->name, step, errno);
                _exit(126);
            }
        }
        svc_chdir(svc, WD_MAIN);
        if (svc->exec_pre_count) {
            svc_apply_env(svc, file_env, file_envc);
            svc_run_pre(svc, 0);
        }
        if (svc->notify || svc->fdstore_max > 0) setenv("NOTIFY_SOCKET", service_notify_sock, 1);
        else unsetenv("NOTIFY_SOCKET");
        if (svc->notify && svc->watchdog_sec > 0) {
            char wv[24];
            snprintf(wv, sizeof wv, "%lld", (long long)svc->watchdog_sec * 1000000);
            setenv("WATCHDOG_USEC", wv, 1);
            snprintf(wv, sizeof wv, "%d", (int)getpid());
            setenv("WATCHDOG_PID", wv, 1);
        } else {
            unsetenv("WATCHDOG_USEC");
            unsetenv("WATCHDOG_PID");
        }
        svc_apply_env(svc, file_env, file_envc);
        int stored = svc_pass_listen(svc);
        if (syscall(SYS_close_range, 3 + svc->listen_open + stored, ~0U, 0) < 0)
            close_fds_from(3 + svc->listen_open + stored);
        char **argv = svc->argv, *xargv[64];
        if (svc->expand_args) {
            int nx = service_expand_argv(svc->argv, xargv, 63);
            if (nx < 0) {
                dprintf(2, "[schema-init] %s: args expand to more than 63 words\n", svc->name);
                _exit(1);
            }
            xargv[nx] = NULL;
            argv = xargv;
        }
        svc_seccomp_or_die(svc, "");
        execv(svc->exec, argv);
        _exit(127);
    }

    close(sync[0]);
    svc->child_pid  = pid;
    if (svc->exec_dir_count[EXEC_DIR_RUNTIME]) svc->rt_made = 1;
    svc->fork_state = 0;
    svc->main_pid = 0;
    svc->main_wait = 0;
    svc->last_start = time(NULL);
    svc->start_time = svc->last_start;
    clock_gettime(CLOCK_MONOTONIC, &svc->spawn_time_mono);
    svc->wd_fired = 0;
    svc->kill_sent = (struct timespec){0, 0};
    if (!svc->boot_spawn.tv_sec && !svc->boot_spawn.tv_nsec)
        svc->boot_spawn = svc->spawn_time_mono;
    svc->last_pet   = svc->spawn_time_mono;
    svc->wd_abort_at = (struct timespec){0, 0};
    svc->wd_armed_sec = svc->notify && svc->watchdog_sec > 0 ? svc->watchdog_sec : 0;
    svc->notify_ready = 0;
    svc->notify_status[0] = '\0';
    if (service_cgroup_root[0]) cgroup_assign(svc, pid);
    if (!svc->cgroup_path[0] && (service_verifies_limits(svc) || (svc->flags & SVC_MAIN_PID_NOTIFY))) {
        /* nothing to check limits or MAINPID= against: never let it start */
        fprintf(stderr, "[schema-init] %s: no cgroup — limits and MAINPID= cannot be checked, not started\n", svc->name);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(sync[1]);
        svc->child_pid = 0;
        return -1;
    }
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

/* SIGKILL everything in a cgroup. Returns 0 when neither file could be
 * opened, so the caller knows nothing was signalled. */
int cgroup_kill_path(const char *cg) {
    char path[320];
    FILE *f;
    pid_t p;
    int fd;

    /* Linux 5.14+: write 1 to cgroup.kill nukes the whole subtree */
    snprintf(path, sizeof(path), "%s/cgroup.kill", cg);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, "1", 1);
        close(fd);
        return 1;
    }
    /* fallback: read cgroup.procs and kill each PID individually */
    snprintf(path, sizeof(path), "%s/cgroup.procs", cg);
    if (!(f = fopen(path, "r"))) return 0;
    while (fscanf(f, "%d", &p) == 1)
        if (p > 1) kill(p, SIGKILL);
    fclose(f);
    return 1;
}

int cgroup_has_children(const char *cg) {
    DIR *d = opendir(cg);
    struct dirent *e;
    int any = 0;
    if (!d) return 0;
    while (!any && (e = readdir(d)))
        any = e->d_type == DT_DIR && e->d_name[0] != '.';
    closedir(d);
    return any;
}

static int cg_read(const char *dir, const char *file, char *buf, size_t n) {
    char p[600];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    if (!fgets(buf, (int)n, f)) buf[0] = '\0';
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 1;
}

/* memory.max is rounded down to a page by the kernel; only a number gets
 * that slack, never "max", an empty read or a missing file */
static int verify_one(const char *dir, const char *file, const char *want, char *why, size_t n) {
    char got[64];
    if (!want[0]) return 1;
    if (!cg_read(dir, file, got, sizeof got)) snprintf(got, sizeof got, "missing");
    int ok = !strcmp(got, want);
    if (!ok && !strcmp(file, "memory.max") && got[0] && strspn(got, "0123456789") == strlen(got)) {
        unsigned long long w = strtoull(want, NULL, 10), g = strtoull(got, NULL, 10);
        ok = g <= w && w - g < (unsigned long long)sysconf(_SC_PAGESIZE);
    }
    if (!ok) snprintf(why, n, "%s want=%s got=%s", file, want, got);
    return ok;
}

#define VERIFY_MISS_MAX 4
typedef struct { int n; char e[VERIFY_MISS_MAX][144]; } verify_miss_t;

/* Every populated cgroup's first mismatch is kept: which one should hold the
 * limits isn't knowable here (podman's split puts conmon in runtime/, never
 * limited, beside libpod-payload-<id>), so none is allowed to hide another. */
static int verify_walk(const char *cg, const service_t *svc, verify_miss_t *miss) {
    DIR *d = opendir(cg);
    struct dirent *e;
    char sub[512], ev[32], m[96];
    int found = 0;
    if (!d) return 0;
    while (!found && (e = readdir(d))) {
        if (e->d_type != DT_DIR || e->d_name[0] == '.') continue;
        if (snprintf(sub, sizeof sub, "%s/%s", cg, e->d_name) >= (int)sizeof sub) continue;
        /* first line of cgroup.events; an empty subtree can't hold the payload */
        if (!cg_read(sub, "cgroup.events", ev, sizeof ev) || strcmp(ev, "populated 1") != 0) continue;
        found = verify_one(sub, "memory.max", svc->verify_memory_max, m, sizeof m) &&
                verify_one(sub, "cpu.max", svc->verify_cpu_max, m, sizeof m) &&
                verify_one(sub, "pids.max", svc->verify_pids_max, m, sizeof m);
        /* a payload id is ~80 chars: 40 tells runtime/ from libpod-payload-<id> */
        if (!found && miss->n < VERIFY_MISS_MAX)
            snprintf(miss->e[miss->n++], sizeof miss->e[0], "%.40s: %s", e->d_name, m);
        found = found || verify_walk(sub, svc, miss);
    }
    closedir(d);
    return found;
}

static int miss_cmp(const void *a, const void *b) { return strcmp(a, b); }

int verify_limits_match(const char *cg, const service_t *svc, char *why, size_t n) {
    verify_miss_t miss = { 0 };
    size_t off = 0;
    if (verify_walk(cg, svc, &miss)) return 1;
    snprintf(why, n, "no populated cgroup below the service");
    /* sorted: the same tree reads the same whatever order readdir gave */
    qsort(miss.e, (size_t)miss.n, sizeof miss.e[0], miss_cmp);
    for (int i = 0; i < miss.n && off < n; i++)
        off += (size_t)snprintf(why + off, n - off, "%s%s", i ? "; " : "", miss.e[i]);
    return 0;
}

void cgroup_rmdir_tree(const char *cg) {
    DIR *d = opendir(cg);
    if (d) {
        struct dirent *e;
        char sub[512];
        while ((e = readdir(d))) {
            if (e->d_type != DT_DIR || e->d_name[0] == '.') continue;
            if (snprintf(sub, sizeof sub, "%s/%s", cg, e->d_name) < (int)sizeof sub)
                cgroup_rmdir_tree(sub);
        }
        closedir(d);
    }
    rmdir(cg);
}

void service_cgroup_release_partition(service_t *svc) {
    char path[320];
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
        char excl_path[320];
        snprintf(excl_path, sizeof excl_path, "%s/cpuset.cpus.exclusive",
                 service_cgroup_root);
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
}

void service_cgroup_kill(service_t *svc) {
    if (!svc->cgroup_path[0]) return;
    service_cgroup_release_partition(svc);
    cgroup_kill_path(svc->cgroup_path);
    /* a service may have nested cgroups of its own (podman --cgroups=split:
     * <svc>/runtime, <svc>/libpod-payload-*); rmdir of the top alone fails */
    cgroup_rmdir_tree(svc->cgroup_path);
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

    if (service_log_dir) {
        char path[320];
        mkdir(service_log_dir, 0700);
        snprintf(path, sizeof path, "%s/rail.log", service_log_dir);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0640);
        if (fd < 0) return;
    } else {
        mkdir("/var/log/schema-init", 0755);
        fd = open("/var/log/schema-init/rail.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0640);
    }
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

void (*service_log_hook)(const service_t *svc, const char *event, const char *line, size_t n);

void service_rail_note(const char *msg) {
    char line[512];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int n = snprintf(line, sizeof line, "[%04d-%02d-%02d %02d:%02d:%02d] rail: %s\n", t->tm_year + 1900, t->tm_mon + 1,
                     t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, msg);
    if (n > 0) rail_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

void service_log(const service_t *svc, const char *event) {
    char line[512];
    int n;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    /* the event is bounded so a long one (limits-mismatch) can't cut off state/pid */
    n = snprintf(line, sizeof(line),
                 "[%04d-%02d-%02d %02d:%02d:%02d] %-20s  %-12.300s  state=%-12s  wt=%d  pid=%d\n",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec,
                 svc->name, event,
                 state_name(svc->inst.state),
                 svc->inst.weight,
                 (int)svc->child_pid);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 1) {
        n = (int)sizeof(line) - 1;
        line[n - 1] = '\n';   /* a cut line still ends: the next one starts on its own */
    }

    if (service_log_hook)
        service_log_hook(svc, event, line, (size_t)n);
    else {
        fputs(line, stdout);
        fflush(stdout);
    }

    rail_write(line, (size_t)n);

    if (rec_forward_kind(event)) {
        rec_event_t e;
        memset(&e, 0, sizeof e);
        e.src = REC_SRC_PID1;
        rec_copy(e.kind, sizeof e.kind, event);
        rec_copy(e.subj, sizeof e.subj, svc->name);
        e.pid = (int32_t)(svc->child_pid ? svc->child_pid : svc->last_pid);
        e.rc = svc->term_signal ? 128 + svc->term_signal : svc->exit_status;
        e.crit = !!(svc->flags & SVC_CRITICAL);
        rec_send(REC_SOCK_PATH, &e);
    }
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
            if (s == STATE_EXCISED || s == STATE_DORMANT) {
                if (stable[di].flags & SVC_CRITICAL) return 0;
                continue;   /* non-critical excised/dormant dep: proceed without it */
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

static uint32_t fnv1a_bytes(uint32_t h, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= ((const unsigned char *)p)[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t fnv1a_file_from(uint32_t h, const char *path) {
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

/* The base file's bytes alone (unchanged from before drop-ins, so a host
 * with none keeps the same hash), then each drop-in's path, NUL, bytes. */
static uint32_t svc_content_hash(const char *path, char (*dropins)[SVC_DROPIN_PATH], int n) {
    uint32_t h = fnv1a_file_from(2166136261u, path);
    for (int i = 0; i < n && h; i++) {
        h = fnv1a_bytes(h, dropins[i], strlen(dropins[i]) + 1);
        h = fnv1a_file_from(h, dropins[i]);
    }
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

static int cg_key_int(const service_t *svc, const char *path, const char *key,
                      const char *val, long max, const char *want) {
    char *end;
    long l = strtol(val, &end, 10);
    if (*end || l < 1 || l > max) {
        fprintf(stderr, "[schema-init] %s: %s=%s must be %s\n", svc->name[0] ? svc->name : path, key, val, want);
        return -1;
    }
    return 0;
}

/* "512", "512M", "2G"; empty or 0 = no limit */
static int mem_limit_parse(const char *val, long *mb) {
    char *end;
    long v;
    if (!*val) { *mb = 0; return 0; }
    if (!isdigit((unsigned char)*val)) return -1;
    errno = 0;
    v = strtol(val, &end, 10);
    if (errno || v < 0) return -1;
    if ((*end == 'G' || *end == 'g') && !end[1]) {
        if (v > LONG_MAX / 1024) return -1;
        v *= 1024;
    } else if (!((*end == 'M' || *end == 'm') && !end[1]) && *end) {
        return -1;
    }
    if (v > MEM_LIMIT_MB_MAX) return -1;
    *mb = v;
    return 0;
}

static int parse_partition(const char *val) {
    if (strcasecmp(val, "isolated") == 0) return PART_ISOLATED;
    if (strcasecmp(val, "root") == 0)     return PART_ROOT;
    return PART_MEMBER;
}

struct parse_ctx {
    int argc;
    int dep_slot;
    int dropin;
    int cal_file;   /* an on_calendar= line seen in the file being parsed */
};

/* each on_calendar= line adds a schedule (at most 4); the first one in a
 * drop-in replaces those from earlier files, so a drop-in still overrides */
static int parse_calendar(service_t *svc, struct parse_ctx *pc, const char *val) {
    cal_spec_t c;
    if (calendar_parse(val, &c) != 0) return -1;
    if (!pc->cal_file) {
        pc->cal_file = 1;
        if (pc->dropin) svc->timer_cal_n = 0;
    }
    if (svc->timer_cal_n >= 4) return -1;
    /* a typo must not schedule garbage, nor a spec that never occurs */
    if (!calendar_next_after(&c, 1, time(NULL))) return -1;
    svc->timer_cal[svc->timer_cal_n++] = c;
    svc->flags |= SVC_TIMER | SVC_TIMER_CALENDAR | SVC_ONESHOT;
    return 0;
}

static void svc_init_defaults(service_t *svc) {
    memset(svc, 0, sizeof(*svc));
    for (int i = 0; i < MAX_DEPS; i++) svc->dep_idx[i] = -1;
    for (int i = 0; i < MAX_DEPS; i++) svc->grp_dep_idx[i] = -1;
    schema_instance_init(&svc->inst, 0, STATE_PERFECT);
    svc->stable_secs = STABLE_SECS;
    svc->priority = PRIO_STANDARD;
    svc->start_timeout_sec = -1;
    svc->umask_val = -1;
    svc->io_class = svc->io_prio = svc->sched_policy = -1;
    svc->allowed_slot_min = -1;
    svc->allowed_slot_max = -1;
    svc->max_restarts = MAX_RESTARTS;
    svc->landlock_tcp_n[0] = svc->landlock_tcp_n[1] = -1;
    svc->directory_mode = 0755;
    svc->trigger_burst = 200;
    svc->trigger_interval_sec = 2;
}

static void svc_free_strings(service_t *svc) {
    if (svc->argv[0] != svc->exec) free(svc->argv[0]);
    svc->argv[0] = NULL;
    for (int i = 1; i < MAX_ARGV; i++) {
        free(svc->argv[i]);
        svc->argv[i] = NULL;
    }
    for (int i = 0; i < svc->env_count; i++) {
        free(svc->envp[i]);
        svc->envp[i] = NULL;
    }
    svc->env_count = 0;
    service_free_landlock(svc);
}

static void svc_free_cmds(char **cmds, int *n) {
    for (int i = 0; i < *n; i++) {
        free(cmds[i]);
        cmds[i] = NULL;
    }
    *n = 0;
}

static void rw_path_clear(service_t *svc) {
    for (int i = 0; i < svc->rw_path_count; i++) {
        free(svc->rw_path[i]);
        svc->rw_path[i] = NULL;
    }
    svc->rw_path_count = 0;
}

/* absolute, not "/", no ".." component; surrounding blanks and trailing '/'
 * stripped in place: a stray space would name a missing path, silently skipped */
static int rw_path_ok(char *v) {
    size_t n = strlen(v);
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) v[--n] = '\0';
    while (n > 1 && v[n - 1] == '/') v[--n] = '\0';
    if (v[0] != '/' || n < 2) return 0;
    for (const char *p = v; (p = strstr(p, "..")); p += 2)
        if (p[-1] == '/' && (p[2] == '/' || p[2] == '\0')) return 0;
    return 1;
}

const char *const service_exec_dir_key[EXEC_DIR_KINDS] = { "runtime_dir", "state_dir", "cache_dir", "logs_dir" };

static void exec_dir_clear(service_t *svc, int k) {
    for (int i = 0; i < svc->exec_dir_count[k]; i++) {
        free(svc->exec_dir[k][i]);
        svc->exec_dir[k][i] = NULL;
    }
    svc->exec_dir_count[k] = 0;
}

static int exec_dir_kind(const char *key, int mode) {
    for (int k = 0; k < EXEC_DIR_KINDS; k++) {
        size_t n = strlen(service_exec_dir_key[k]);
        if (strncmp(key, service_exec_dir_key[k], n) == 0 && strcmp(key + n, mode ? "_mode" : "") == 0) return k;
    }
    return -1;
}

/* relative, no empty/./.. component, no ':' (systemd's dir:symlink form);
 * one trailing '/' is stripped in place */
static int exec_dir_ok(char *v) {
    size_t n = strlen(v);
    if (n > 1 && v[n - 1] == '/') v[--n] = '\0';
    if (!n || n > 200 || v[0] == '/' || strchr(v, ':')) return 0;
    for (const char *c = v; ; ) {
        size_t l = strcspn(c, "/");
        if (l == 0 || (l == 1 && c[0] == '.') || (l == 2 && c[0] == '.' && c[1] == '.')) return 0;
        if (!c[l]) return 1;
        c += l + 1;
    }
}

const char *service_exec_dir_root;

int service_exec_dir_base(int kind, char *out, size_t n) {
    static const char *const sys[EXEC_DIR_KINDS] = { "/run", "/var/lib", "/var/cache", "/var/log" };
    static const char *const xdg[EXEC_DIR_KINDS] = { "XDG_RUNTIME_DIR", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME" };
    static const char *const home_rel[EXEC_DIR_KINDS] = { NULL, "/.local/state", "/.cache", "/.local/state" };
    const char *x, *rel = "";
    int r;
    if (kind < 0 || kind >= EXEC_DIR_KINDS) return -1;
    if (geteuid() == 0) {
        r = snprintf(out, n, "%s%s", service_exec_dir_root ? service_exec_dir_root : "", sys[kind]);
        return r < 0 || (size_t)r >= n ? -1 : 0;
    }
    /* schema-user: where systemd's user manager puts them */
    x = getenv(xdg[kind]);
    if (!x || x[0] != '/') {
        if (!home_rel[kind] || !(x = getenv("HOME")) || x[0] != '/') return -1;
        rel = home_rel[kind];
    }
    r = snprintf(out, n, "%s%s%s", x, rel, kind == EXEC_DIR_LOGS ? "/log" : "");
    return r < 0 || (size_t)r >= n ? -1 : 0;
}

/* the base itself is trusted (root's /var/lib, or the user's own dirs, which
 * may sit under a symlinked /home): created like mkdir -p, following links */
static int mkdir_base(const char *base) {
    char b[PATH_MAX];
    if (strlen(base) >= sizeof b) { errno = ENAMETOOLONG; return -1; }
    strcpy(b, base);
    for (char *c = b + 1; ; c++) {
        if (*c == '/' || !*c) {
            char save = *c;
            *c = '\0';
            if (mkdir(b, 0755) != 0 && errno != EEXIST) return -1;
            if (!(*c = save)) return 0;
        }
    }
}

/* a directory only the creator (or root) can write: a symlink in it was put
 * there by an admin, not by the service user */
static int dir_trusted(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && (st.st_uid == 0 || st.st_uid == geteuid()) && !(st.st_mode & 022);
}

/* open NAME in FD as a directory: never through a symlink, unless FOLLOW and
 * FD is trusted (an admin's /var/lib/mysql -> /data/mysql) */
static int open_dir_at(int fd, const char *name, int follow) {
    int nfd = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    int e = errno;
    if (nfd >= 0 || !follow || (e != ELOOP && e != ENOTDIR)) return nfd;
    if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISLNK(st.st_mode)) {
        errno = e;
        return -1;
    }
    if (!dir_trusted(fd)) {
        errno = ELOOP;
        return -1;
    }
    return openat(fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

/* every component below BASE through open_dir_at: a parent the service user
 * owns can hold a planted symlink, and root must not follow it. MAKE: create
 * missing parents (root-owned 0755) */
static int walk_parent(const char *base, char *rel, char **leaf, int make, const char **step) {
    int fd;
    *step = "open base";
    fd = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    *leaf = rel;
    for (char *c = rel, *sl; (sl = strchr(c, '/')); c = sl + 1) {
        *sl = '\0';
        *step = "mkdir";
        if (make && mkdirat(fd, c, 0755) != 0 && errno != EEXIST) { *sl = '/'; goto fail; }
        *step = "open";
        int nfd = open_dir_at(fd, c, make);
        *sl = '/';
        if (nfd < 0) goto fail;
        close(fd);
        fd = nfd;
        *leaf = sl + 1;
    }
    return fd;
fail: {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
}

int service_make_dir(const char *base, const char *rel, mode_t mode, uid_t uid, gid_t gid,
                     const char **step) {
    char buf[256], *leaf;
    int pfd, fd = -1, e;
    if (strlen(rel) >= sizeof buf) { *step = "path"; errno = ENAMETOOLONG; return -1; }
    strcpy(buf, rel);
    *step = "mkdir base";
    if (mkdir_base(base) != 0) return -1;
    pfd = walk_parent(base, buf, &leaf, 1, step);
    if (pfd < 0) return -1;
    /* 0700 until the owner and mode are set */
    *step = "mkdir";
    if (mkdirat(pfd, leaf, 0700) != 0 && errno != EEXIST) goto out;
    *step = "open";
    fd = open_dir_at(pfd, leaf, 1);
    if (fd < 0) goto out;
    *step = "chown";
    if (fchown(fd, uid, gid) != 0) goto out;
    *step = "chmod";
    if (fchmod(fd, mode) != 0) goto out;
    close(fd);
    close(pfd);
    return 0;
out:
    e = errno;
    if (fd >= 0) close(fd);
    close(pfd);
    errno = e;
    return -1;
}

/* re-own a dynamic user's dir after its uid changed: never through a
 * symlink (lchown only), never into another mount (a planted bind would
 * hand root's chown host files) */
static int chown_tree(int fd, uid_t uid, gid_t gid, int depth) {
    struct stat ds;
    int r = 0;
    /* children first, the top last: a re-own cut short leaves the top on the
     * old uid, so the next start tries again */
    if (depth > 256) { errno = ELOOP; return -1; }
    if (fstat(fd, &ds) != 0) return -1;
    int dfd = dup(fd);
    DIR *d = dfd < 0 ? NULL : fdopendir(dfd);
    if (!d) {
        if (dfd >= 0) close(dfd);
        return -1;
    }
    for (struct dirent *e; (e = readdir(d));) {
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || st.st_dev != ds.st_dev) continue;
        if (!S_ISDIR(st.st_mode)) {
            if (fchownat(fd, e->d_name, uid, gid, AT_SYMLINK_NOFOLLOW) != 0) r = -1;
            continue;
        }
        int c = openat(fd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (c < 0) continue;
        struct stat cs;
        if (fstat(c, &cs) == 0 && cs.st_dev == ds.st_dev && chown_tree(c, uid, gid, depth + 1) != 0) r = -1;
        close(c);
    }
    closedir(d);
    if (r == 0 && fchown(fd, uid, gid) != 0) r = -1;
    return r;
}

/* dynamic_user: the dir lives at <base>/private/<d> (private root 0700) and
 * <base>/<d> is a symlink to it; a plain <base>/<d> from before moves in */
static int svc_dynamic_dir(const char *base, const char *d, mode_t mode, uid_t uid, gid_t gid,
                           const char **step) {
    char priv[PATH_MAX + 16], tgt[300], link[300];
    struct stat st;
    int bfd = -1, pfd = -1, fd = -1, r = -1;
    if (snprintf(priv, sizeof priv, "%s/private", base) >= (int)sizeof priv) { *step = "path"; return -1; }
    snprintf(tgt, sizeof tgt, "private/%s", d);
    *step = "mkdir base";
    if (mkdir_base(base) != 0) return -1;
    *step = "private";
    if (mkdir(priv, 0700) != 0 && errno != EEXIST) return -1;
    if ((bfd = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) return -1;
    if ((pfd = openat(bfd, "private", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0) goto out;
    if (fchown(pfd, 0, 0) != 0 || fchmod(pfd, 0700) != 0) goto out;
    *step = "migrate";
    if (fstatat(bfd, d, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode) &&
        renameat(bfd, d, pfd, d) != 0)
        goto out;
    *step = "owner";
    fd = openat(pfd, d, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_uid != uid && chown_tree(fd, uid, gid, 0) != 0) goto out;
    if (service_make_dir(priv, d, mode, uid, gid, step) != 0) goto out;
    *step = "symlink";
    ssize_t n = readlinkat(bfd, d, link, sizeof link - 1);
    if (n >= 0) {
        link[n] = '\0';
        if (strcmp(link, tgt) != 0 && (unlinkat(bfd, d, 0) != 0 || symlinkat(tgt, bfd, d) != 0)) goto out;
    } else if (errno != ENOENT || symlinkat(tgt, bfd, d) != 0) {
        goto out;
    }
    r = 0;
out:
    if (fd >= 0) close(fd);
    if (pfd >= 0) close(pfd);
    close(bfd);
    return r;
}

/* In the service's own mount namespace: a tmpfs over each <base>/private
 * with only this service's dirs bound back, so another service handed the
 * same uid later never reaches them. The tmpfs hides the host's dirs by
 * path, so each is held O_PATH first (the private_devices pattern). */
int service_private_binds(const service_t *svc) {
    int fd[3][MAX_EXEC_DIRS], r = -1, any = 0;
    char base[3][PATH_MAX], p[PATH_MAX + 320];
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < MAX_EXEC_DIRS; i++) fd[k][i] = -1;
    for (int k = 0; k < 3; k++) any |= svc->exec_dir_count[EXEC_DIR_STATE + k];
    if (!any) return 0;
    /* the fds are taken inside the new namespace: a bind source from another
     * mount namespace is refused (EINVAL) */
    if (unshare(CLONE_NEWNS) != 0 || mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL) != 0) return -1;
    for (int k = 0; k < 3; k++) {
        int kind = EXEC_DIR_STATE + k;
        if (!svc->exec_dir_count[kind]) continue;
        if (service_exec_dir_base(kind, base[k], sizeof base[k]) != 0) goto out;
        for (int i = 0; i < svc->exec_dir_count[kind]; i++) {
            if (snprintf(p, sizeof p, "%s/private/%s", base[k], svc->exec_dir[kind][i]) >= (int)sizeof p) goto out;
            if ((fd[k][i] = open(p, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0) goto out;
        }
    }
    for (int k = 0; k < 3; k++) {
        int kind = EXEC_DIR_STATE + k;
        if (!svc->exec_dir_count[kind]) continue;
        if (snprintf(p, sizeof p, "%s/private", base[k]) >= (int)sizeof p) goto out;
        if (mount("tmpfs", p, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0755,size=64k") != 0) goto out;
        for (int i = 0; i < svc->exec_dir_count[kind]; i++) {
            char src[32];
            if (snprintf(p, sizeof p, "%s/private/%s", base[k], svc->exec_dir[kind][i]) >= (int)sizeof p) goto out;
            snprintf(src, sizeof src, "/proc/self/fd/%d", fd[k][i]);
            if ((mkdir(p, 0755) != 0 && errno != EEXIST) || mount(src, p, NULL, MS_BIND | MS_REC, NULL) != 0) goto out;
        }
    }
    r = 0;
out:
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < MAX_EXEC_DIRS; i++)
            if (fd[k][i] >= 0) close(fd[k][i]);
    return r;
}

static int rm_at(int dfd, const char *name, int depth) {
    if (unlinkat(dfd, name, 0) == 0 || errno == ENOENT) return 0;
    if (errno != EISDIR && errno != EPERM) return -1;
    if (depth > 256) { errno = ELOOP; return -1; }
    int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    /* never into another mount (a bind or FUSE mount the service left below
     * its runtime dir): its files are not ours to delete */
    struct statx sx;
    struct stat ps, cs;
    if (fstat(dfd, &ps) != 0 || fstat(fd, &cs) != 0 || ps.st_dev != cs.st_dev ||
        (statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx) == 0 &&
         (sx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) && (sx.stx_attributes & STATX_ATTR_MOUNT_ROOT))) {
        close(fd);
        errno = EBUSY;
        return -1;
    }
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return -1; }
    /* entries removed while reading may hide others: read again until a pass removes nothing */
    for (int pass = 0, any = 1; any && pass < 8; pass++) {
        struct dirent *de;
        any = 0;
        rewinddir(d);
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            if (rm_at(dirfd(d), de->d_name, depth + 1) == 0) any = 1;
        }
    }
    closedir(d);
    return unlinkat(dfd, name, AT_REMOVEDIR) == 0 || errno == ENOENT ? 0 : -1;
}

int service_remove_tree(const char *base, const char *rel) {
    char buf[256], *leaf;
    const char *step;
    int pfd, r;
    if (strlen(rel) >= sizeof buf) return -1;
    strcpy(buf, rel);
    /* no mkdir on the way: walk_parent creates only what's missing, and a
     * missing parent means nothing to remove */
    for (char *c = buf, *sl; (sl = strchr(c, '/')); c = sl + 1) {
        char save = *sl;
        struct stat st;
        char full[PATH_MAX + 256];
        *sl = '\0';
        snprintf(full, sizeof full, "%s/%s", base, buf);
        *sl = save;
        if (lstat(full, &st) != 0 && errno == ENOENT) return 0;
    }
    pfd = walk_parent(base, buf, &leaf, 0, &step);
    if (pfd < 0) return -1;
    r = rm_at(pfd, leaf, 0);
    close(pfd);
    return r;
}

static int str_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

size_t service_exec_dir_env_size(const service_t *svc, int kind, const char *base) {
    size_t n = 1;
    for (int i = 0; i < svc->exec_dir_count[kind]; i++)
        n += strlen(base) + strlen(svc->exec_dir[kind][i]) + 2;
    return n;
}

int service_exec_dir_env(const service_t *svc, int kind, const char *base, char *out, size_t n) {
    char *s[MAX_EXEC_DIRS];
    size_t len = 0;
    int c = svc->exec_dir_count[kind];
    memcpy(s, svc->exec_dir[kind], c * sizeof *s);
    qsort(s, c, sizeof *s, str_cmp);
    out[0] = '\0';
    for (int i = 0; i < c; i++) {
        int r = snprintf(out + len, n - len, "%s%s/%s", i ? ":" : "", base, s[i]);
        if (r < 0 || (size_t)r >= n - len) return -1;
        len += r;
    }
    return 0;
}

/* in the spawn child, before the root exec_pre lines (systemd creates them
 * before ExecStartPre=); exports RUNTIME_DIRECTORY= etc. CREATE 0: the
 * variables only, for the stop and reload runners */
static void svc_make_exec_dirs(const service_t *svc, int create) {
    static const char *const var[EXEC_DIR_KINDS] = { "RUNTIME_DIRECTORY", "STATE_DIRECTORY",
                                                     "CACHE_DIRECTORY", "LOGS_DIRECTORY" };
    int root = geteuid() == 0;
    uid_t uid = root ? svc->run_uid : (uid_t)-1;
    gid_t gid = root && (svc->run_uid || svc->run_group[0]) ? svc->run_gid : root ? 0 : (gid_t)-1;
    /* systemd still gives the dirs to User= when ExecStart=!/+ runs the
     * process as root (Debian's chrony): looked up at each start, like user= */
    /* never over a dynamic user: its private dirs must stay the allocated uid's */
    if (root && create && svc->dir_owner[0] && !svc->dynamic_user) {
        char u[64], *g = NULL, *end;
        strcpy(u, svc->dir_owner);
        if ((g = strchr(u, ':'))) *g++ = '\0';
        errno = 0;
        unsigned long id = strtoul(u, &end, 10);
        struct passwd *pw = *end ? getpwnam(u) : !errno && id < (uid_t)-1 ? getpwuid((uid_t)id) : NULL;
        if (!pw || (g && *g && !svc_group_resolve(g, &gid))) {
            dprintf(2, "[schema-init] %s: dir_owner=%s: no such %s\n", svc->name, svc->dir_owner,
                    pw ? "group" : "user");
            _exit(126);
        }
        uid = pw->pw_uid;
        if (!g || !*g) gid = pw->pw_gid;
    }
    for (int k = 0; k < EXEC_DIR_KINDS; k++) {
        char base[PATH_MAX], *env;
        const char *step = "";
        mode_t mode = svc->exec_dir_mode_set & (1 << k) ? svc->exec_dir_mode[k] : 0755;
        if (!svc->exec_dir_count[k]) continue;
        if (service_exec_dir_base(k, base, sizeof base) != 0) {
            if (!create) continue;
            dprintf(2, "[schema-init] %s: %s=: no base directory (XDG_RUNTIME_DIR/HOME unset)\n",
                    svc->name, service_exec_dir_key[k]);
            _exit(126);
        }
        int dyn = svc->dynamic_user && root && k != EXEC_DIR_RUNTIME;
        for (int i = 0; create && i < svc->exec_dir_count[k]; i++) {
            if ((dyn ? svc_dynamic_dir(base, svc->exec_dir[k][i], mode, uid, gid, &step)
                     : service_make_dir(base, svc->exec_dir[k][i], mode, uid, gid, &step)) == 0) continue;
            dprintf(2, "[schema-init] %s: %s=%s: %s: %s%s\n", svc->name, service_exec_dir_key[k],
                    svc->exec_dir[k][i], step, strerror(errno),
                    k == EXEC_DIR_RUNTIME || dyn ? "" : " (starting anyway)");
            /* the daemon can't run without its runtime dir, nor a dynamic user
             * without its private dirs (systemd fails those starts too); the
             * others may already be there in a form we won't touch */
            if (k == EXEC_DIR_RUNTIME || dyn) _exit(126);
        }
        size_t need = service_exec_dir_env_size(svc, k, base);
        if ((env = malloc(need)) && service_exec_dir_env(svc, k, base, env, need) == 0) setenv(var[k], env, 1);
        free(env);
    }
}

void service_runtime_dirs_remove(const service_t *svc) {
    char base[PATH_MAX], aside[MAX_EXEC_DIRS][96];
    int n = 0, bfd;
    if (service_exec_dir_base(EXEC_DIR_RUNTIME, base, sizeof base) != 0) return;
    bfd = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (bfd < 0) return;
    /* renamed aside at once, so a start right after gets a fresh dir and
     * never races the delete; the delete runs in a child reap() collects */
    for (int i = 0; i < svc->exec_dir_count[EXEC_DIR_RUNTIME]; i++) {
        char buf[256], *leaf;
        const char *step;
        int pfd;
        snprintf(buf, sizeof buf, "%s", svc->exec_dir[EXEC_DIR_RUNTIME][i]);
        if (strchr(buf, '/')) {
            struct stat st;
            char full[PATH_MAX + 256];
            *strrchr(buf, '/') = '\0';
            snprintf(full, sizeof full, "%s/%s", base, buf);
            if (lstat(full, &st) != 0) continue;
            snprintf(buf, sizeof buf, "%s", svc->exec_dir[EXEC_DIR_RUNTIME][i]);
        }
        pfd = walk_parent(base, buf, &leaf, 0, &step);
        if (pfd < 0) continue;
        /* unique per call: getpid() is 1 in PID 1, and a delete cut short by a
         * shutdown leaves its aside behind */
        struct timespec ts;
        static unsigned seq;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        snprintf(aside[n], sizeof aside[n], ".schema-gone-%.40s-%lld%09ld-%u", svc->name,
                 (long long)ts.tv_sec, ts.tv_nsec, seq++);
        if (renameat(pfd, leaf, bfd, aside[n]) == 0) n++;
        else if (errno != ENOENT)
            fprintf(stderr, "[schema-init] %s: runtime_dir=%s not removed: %s\n", svc->name,
                    svc->exec_dir[EXEC_DIR_RUNTIME][i], strerror(errno));
        close(pfd);
    }
    close(bfd);
    if (!n) return;
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < n; i++) service_remove_tree(base, aside[i]);
        _exit(0);
    }
}

void service_free_landlock(service_t *svc) {
    svc_free_cmds(svc->exec_pre, &svc->exec_pre_count);
    svc_free_cmds(svc->exec_stop, &svc->exec_stop_count);
    svc_free_cmds(svc->exec_stop_post, &svc->exec_stop_post_count);
    svc_free_cmds(svc->exec_reload, &svc->exec_reload_count);
    for (int i = 0; i < svc->card_count; i++) {
        free(svc->card[i]);
        svc->card[i] = NULL;
    }
    svc->card_count = 0;
    for (int i = 0; i < svc->landlock_count; i++) {
        free(svc->landlock[i]);
        svc->landlock[i] = NULL;
    }
    svc->landlock_count = 0;
    svc->landlock_rw = 0;
    rw_path_clear(svc);
    for (int k = 0; k < EXEC_DIR_KINDS; k++) exec_dir_clear(svc, k);
}

static void landlock_clear(service_t *svc, int rw) {
    int o = 0;
    uint32_t mask = 0;
    for (int i = 0; i < svc->landlock_count; i++) {
        int is_rw = (svc->landlock_rw >> i) & 1;
        if (is_rw == rw) {
            free(svc->landlock[i]);
            continue;
        }
        if (is_rw) mask |= 1U << o;
        svc->landlock[o++] = svc->landlock[i];
    }
    for (int i = o; i < svc->landlock_count; i++) svc->landlock[i] = NULL;
    svc->landlock_count = o;
    svc->landlock_rw = mask;
}


static int dropin_flag(service_t *svc, const char *key, const char *val) {
    static const struct { const char *key; unsigned flag; } f[] = {
        { "oneshot", SVC_ONESHOT }, { "needs_root", SVC_NEEDS_ROOT },
        { "critical", SVC_CRITICAL }, { "no_restart", SVC_NO_RESTART },
        { "persistent", SVC_TIMER_PERSIST }, { "stop_first", SVC_STOP_FIRST },
        { "main_pid_notify", SVC_MAIN_PID_NOTIFY },
    };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        if (strcmp(key, f[i].key) != 0) continue;
        if (atoi(val)) svc->flags |= f[i].flag;
        else svc->flags &= ~f[i].flag;
        return 1;
    }
    return 0;
}

/* Drop-in only: name= is refused, an empty args=/env=/dep= clears that
 * list, and flag=0 clears the flag. 1 = handled, 0 = not ours, -1 = reject. */
static int svc_parse_dropin_line(service_t *svc, struct parse_ctx *pc, const char *key,
                                 const char *val, const char *path) {
    if (strcmp(key, "name") == 0) {
        fprintf(stderr, "[schema-init] %s: name= is not allowed in a drop-in\n", path);
        return -1;
    }
    if (dropin_flag(svc, key, val)) return 1;
    if (strcmp(key, "no_new_privs") == 0) {
        svc->hard_set |= HARD_NNP;
        if (atoi(val)) svc->flags |= SVC_NO_NEW_PRIVS;
        else svc->flags &= ~SVC_NO_NEW_PRIVS;
        return 1;
    }
    if (val[strspn(val, " \t")] != '\0') return 0;
    if (strcmp(key, "args") == 0) {
        for (int i = 1; i < MAX_ARGV; i++) {
            free(svc->argv[i]);
            svc->argv[i] = NULL;
        }
        pc->argc = 1;
    } else if (strcmp(key, "env") == 0) {
        for (int i = 0; i < svc->env_count; i++) {
            free(svc->envp[i]);
            svc->envp[i] = NULL;
        }
        svc->env_count = 0;
    } else if (strcmp(key, "exec_pre") == 0) {
        svc_free_cmds(svc->exec_pre, &svc->exec_pre_count);
    } else if (strcmp(key, "working_dir") == 0) {
        svc->working_dir[0] = '\0';
    } else if (strcmp(key, "exec_stop") == 0) {
        svc_free_cmds(svc->exec_stop, &svc->exec_stop_count);
    } else if (strcmp(key, "exec_stop_post") == 0) {
        svc_free_cmds(svc->exec_stop_post, &svc->exec_stop_post_count);
    } else if (strcmp(key, "exec_reload") == 0) {
        svc_free_cmds(svc->exec_reload, &svc->exec_reload_count);
    } else if (strcmp(key, "reload_signal") == 0) {
        svc->reload_signal = 0;
        svc->reload_signal_set = 0;
    } else if (strcmp(key, "kill_mode") == 0) {
        svc->kill_mode = KILL_MODE_DEFAULT;
    } else if (strcmp(key, "kill_signal") == 0) {
        svc->kill_signal = 0;
    } else if (strcmp(key, "final_kill_signal") == 0) {
        svc->final_kill_signal = 0;
    } else if (strcmp(key, "restart") == 0) {
        svc->restart_policy = RESTART_ALWAYS;
        svc->flags &= ~SVC_NO_RESTART;
    } else if (strcmp(key, "success_exit_status") == 0) {
        memset(svc->success_codes, 0, sizeof svc->success_codes);
        svc->success_sigs = 0;
    } else if (strcmp(key, "restart_prevent_exit_status") == 0) {
        memset(svc->prevent_codes, 0, sizeof svc->prevent_codes);
        svc->prevent_sigs = 0;
    } else if (strcmp(key, "card") == 0) {
        for (int i = 0; i < svc->card_count; i++) {
            free(svc->card[i]);
            svc->card[i] = NULL;
        }
        svc->card_count = 0;
    } else if (strcmp(key, "condition") == 0) {
        memset(svc->cond, 0, sizeof svc->cond);
        svc->cond_count = 0;
    } else if (strcmp(key, "listen") == 0) {
        memset(svc->listen, 0, sizeof svc->listen);
        svc->listen_count = 0;
    } else if (strcmp(key, "env_file") == 0) {
        memset(svc->env_file, 0, sizeof svc->env_file);
        svc->env_file_count = 0;
    } else if (strcmp(key, "dep") == 0) {
        memset(svc->dep_name, 0, sizeof svc->dep_name);
        pc->dep_slot = 0;
    } else if (strcmp(key, "after") == 0) {
        memset(svc->after_name, 0, sizeof svc->after_name);
    } else if (strcmp(key, "before") == 0) {
        memset(svc->before_name, 0, sizeof svc->before_name);
    } else if (strcmp(key, "bind") == 0) {
        svc->bind_name[0] = '\0';
    } else if (strcmp(key, "landlock_ro") == 0 || strcmp(key, "landlock_rw") == 0) {
        landlock_clear(svc, key[10] == 'w');
    } else if (strcmp(key, "rw_path") == 0) {
        rw_path_clear(svc);
    } else if (exec_dir_kind(key, 0) >= 0) {
        exec_dir_clear(svc, exec_dir_kind(key, 0));
    } else if (strcmp(key, "landlock_bind_tcp") == 0 || strcmp(key, "landlock_connect_tcp") == 0) {
        svc->landlock_tcp_n[key[9] == 'c'] = -1;
    } else
        return 0;
    return 1;
}

const struct service_rlim_key service_rlim_keys[] = {
    { "limit_cpu", RLIMIT_CPU },         { "limit_fsize", RLIMIT_FSIZE },
    { "limit_data", RLIMIT_DATA },       { "limit_stack", RLIMIT_STACK },
    { "limit_core", RLIMIT_CORE },       { "limit_rss", RLIMIT_RSS },
    { "limit_nofile", RLIMIT_NOFILE },   { "limit_as", RLIMIT_AS },
    { "limit_nproc", RLIMIT_NPROC },     { "limit_memlock", RLIMIT_MEMLOCK },
    { "limit_locks", RLIMIT_LOCKS },     { "limit_sigpending", RLIMIT_SIGPENDING },
    { "limit_msgqueue", RLIMIT_MSGQUEUE }, { "limit_nice", RLIMIT_NICE },
    { "limit_rtprio", RLIMIT_RTPRIO },   { "limit_rttime", RLIMIT_RTTIME },
};
_Static_assert(sizeof service_rlim_keys / sizeof *service_rlim_keys == SERVICE_RLIM_KEYS,
               "SERVICE_RLIM_KEYS in service.h must match service_rlim_keys[]");

/* systemd's Limit*= value: a number with an optional K/M/G/T (1024) suffix, or
 * "infinity". limit_nice also takes a signed nice level, -20..19. */
static int rlim_value(const char *s, int nice, rlim_t *out) {
    char *end;
    if (strcmp(s, "infinity") == 0) { *out = RLIM_INFINITY; return 0; }
    if (nice && (*s == '-' || *s == '+')) {
        long n = strtol(s, &end, 10);
        if (*end || n < -20 || n > 19) return -1;
        *out = (rlim_t)(20 - n);
        return 0;
    }
    if (!isdigit((unsigned char)*s)) return -1;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    const char *suf = "KMGT", *k = *end ? strchr(suf, *end) : NULL;
    if (errno || (*end && (!k || end[1]))) return -1;
    if (k) {
        int sh = 10 * (k - suf + 1);
        if (v > (ULLONG_MAX >> sh)) return -1;
        v <<= sh;
    }
    if ((rlim_t)v == RLIM_INFINITY) return -1;
    *out = (rlim_t)v;
    return 0;
}

/* limit_<name>=VALUE or SOFT:HARD. 1 = taken, 0 = not a limit key, -1 = bad value.
 * An empty value drops that limit (drop-ins use this to undo the base file). */
static int svc_parse_rlimit(service_t *svc, const char *key, const char *val) {
    size_t i;
    for (i = 0; i < SERVICE_RLIM_KEYS; i++)
        if (strcmp(key, service_rlim_keys[i].key) == 0) break;
    if (i == SERVICE_RLIM_KEYS) return 0;
    int res = service_rlim_keys[i].res, slot;
    for (slot = 0; slot < svc->rlim_count; slot++)
        if (svc->rlim[slot].res == res) break;
    if (!*val) {
        if (slot < svc->rlim_count)
            svc->rlim[slot] = svc->rlim[--svc->rlim_count];
        return 1;
    }
    char buf[64], *colon;
    rlim_t cur, max;
    snprintf(buf, sizeof buf, "%s", val);
    colon = strchr(buf, ':');
    if (colon) *colon = '\0';
    if (rlim_value(buf, res == RLIMIT_NICE, &cur) != 0) return -1;
    if (colon) {
        if (rlim_value(colon + 1, res == RLIMIT_NICE, &max) != 0) return -1;
        if (max != RLIM_INFINITY && (cur == RLIM_INFINITY || cur > max)) return -1;
    } else
        max = cur;
    svc->rlim[slot].res = res;
    svc->rlim[slot].cur = cur;
    svc->rlim[slot].max = max;
    if (slot == svc->rlim_count) svc->rlim_count++;
    return 1;
}

/* reload_signal=: a name (HUP, SIGHUP, any case) or a number. 0 = refused:
 * KILL and STOP can't be caught, so they'd stop the service, not reload it */
static int svc_signal_name(const char *v) {
    static const struct { const char *n; int s; } t[] = {
        { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "USR1", SIGUSR1 },
        { "USR2", SIGUSR2 }, { "TERM", SIGTERM }, { "CONT", SIGCONT }, { "URG", SIGURG },
        { "WINCH", SIGWINCH }, { "PWR", SIGPWR }, { "ALRM", SIGALRM }, { "TRAP", SIGTRAP },
        { "KILL", SIGKILL }, { "ABRT", SIGABRT }, { "SEGV", SIGSEGV }, { "PIPE", SIGPIPE },
        { "BUS", SIGBUS }, { "FPE", SIGFPE }, { "ILL", SIGILL }, { "SYS", SIGSYS },
    };
    if (strncasecmp(v, "SIG", 3) == 0) v += 3;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (strcasecmp(v, t[i].n) == 0) return t[i].s;
    return 0;
}

/* kill_signal= / final_kill_signal=: any name or 1-64 but STOP, which would
 * only pause it */
static int svc_any_signal_parse(const char *v) {
    while (*v == ' ' || *v == '\t') v++;
    if (*v >= '0' && *v <= '9') {
        char *end;
        long n = strtol(v, &end, 10);
        return *end || n < 1 || n > 64 || n == SIGSTOP ? 0 : (int)n;
    }
    return svc_signal_name(v);
}

/* reload_signal=: a KILL would end the service, not reload it */
static int svc_reload_signal_parse(const char *v) {
    int s = svc_any_signal_parse(v);
    return s == SIGKILL ? 0 : s;
}

/* success_exit_status= / restart_prevent_exit_status=: exit codes 0-255
 * and signal names, space separated; -1 on a token that is neither */
static int svc_status_list(const char *v, uint8_t *codes, uint64_t *sigs) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", v);
    for (char *save, *t = strtok_r(buf, " \t,", &save); t; t = strtok_r(NULL, " \t,", &save)) {
        if (*t >= '0' && *t <= '9') {
            char *end;
            long n = strtol(t, &end, 10);
            if (*end || n > 255) return -1;
            codes[n / 8] |= (uint8_t)(1u << (n % 8));
            continue;
        }
        int sig = svc_signal_name(t);
        if (sig <= 0) return -1;
        *sigs |= 1ULL << (sig - 1);
    }
    return 0;
}

int service_death_class(const service_t *svc) {
    if (svc->wd_fired) return DEATH_WATCHDOG;
    int s = svc->term_signal;
    if (s) {
        /* systemd's clean signals, plus success_exit_status= ones */
        if (s == SIGHUP || s == SIGINT || s == SIGTERM || s == SIGPIPE ||
            (s <= 64 && (svc->success_sigs & (1ULL << (s - 1)))))
            return DEATH_CLEAN;
        return DEATH_SIGNAL;
    }
    int c = svc->exit_status;
    if (c == 0 || (c > 0 && c < 256 && (svc->success_codes[c / 8] & (1u << (c % 8)))))
        return DEATH_CLEAN;
    return DEATH_EXIT;
}

int service_restart_wanted(const service_t *svc, int death) {
    int s = svc->term_signal, c = svc->exit_status;
    if (death != DEATH_TIMEOUT && death != DEATH_WATCHDOG) {
        if (s && s <= 64 && (svc->prevent_sigs & (1ULL << (s - 1)))) return 0;
        if (!s && c >= 0 && c < 256 && (svc->prevent_codes[c / 8] & (1u << (c % 8)))) return 0;
    }
    switch (svc->restart_policy) {
    case RESTART_NO:          return 0;
    case RESTART_ON_SUCCESS:  return death == DEATH_CLEAN;
    case RESTART_ON_FAILURE:  return death != DEATH_CLEAN;
    case RESTART_ON_ABNORMAL: return death == DEATH_SIGNAL || death == DEATH_TIMEOUT || death == DEATH_WATCHDOG;
    case RESTART_ON_ABORT:    return death == DEATH_SIGNAL;
    case RESTART_ON_WATCHDOG: return death == DEATH_WATCHDOG;
    default:                  return 1;
    }
}

/* One key=value line. 0 = ok, -1 = the service must be rejected. */
static int svc_cmd_ok(const char *cmd) {
    char *w[33];
    int nw = service_split_cmdline(cmd, w, 33);
    int ok = nw > 0 && nw <= 32 && w[0][0] == '/';
    for (int i = 0; i < nw; i++) free(w[i]);
    return ok;
}

static int svc_parse_line(service_t *svc, struct parse_ctx *pc, char *line, const char *path) {
    char *eq = strchr(line, '=');
    char *val;
    int nsr;
    if (!eq) return 0;
    *eq = 0;
    val = eq + 1;
    val[strcspn(val, "\r\n")] = 0;

    if (pc->dropin && (nsr = svc_parse_dropin_line(svc, pc, line, val, path)) != 0)
        return nsr < 0 ? -1 : 0;
    if ((nsr = svc_parse_rlimit(svc, line, val)) != 0) {
        if (nsr > 0) return 0;
        fprintf(stderr, "[schema-init] %s: bad %s='%s' (want N, NK/M/G/T, "
                "infinity, or SOFT:HARD)\n", path, line, val);
        return -1;
    }
    if (strcmp(line, "name") == 0)
        strncpy(svc->name, val, sizeof(svc->name) - 1);
    else if (strcmp(line, "exec") == 0) {
        if (strpbrk(val, " \t")) {
            fprintf(stderr, "[schema-init] %s: exec=%s has whitespace; put each argument on its own args= line\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
        strncpy(svc->exec, val, sizeof(svc->exec) - 1);
        if (svc->argv[0] != svc->exec) free(svc->argv[0]);
        svc->argv[0] = svc->exec;
    } else if (strcmp(line, "args") == 0) {
        if (pc->argc >= MAX_ARGV - 1) {
            fprintf(stderr, "[schema-init] %s: more than %d args= lines\n",
                    svc->name[0] ? svc->name : path, MAX_ARGV - 2);
            return -1;
        }
        while (*val == ' ' || *val == '\t') val++;
        svc->argv[pc->argc++] = strdup(val);
    } else if (strcmp(line, "env") == 0) {
        while (*val == ' ' || *val == '\t') val++;
        char *eqv = strchr(val, '=');
        int e = 0;
        if (eqv) {
            size_t kl = (size_t)(eqv - val) + 1;
            while (e < svc->env_count && strncmp(svc->envp[e], val, kl) != 0) e++;
            if (e < svc->env_count) {
                free(svc->envp[e]);
                svc->envp[e] = strdup(val);
            } else if (svc->env_count >= MAX_ENV) {
                fprintf(stderr, "[schema-init] %s: more than %d env= variables\n",
                        svc->name[0] ? svc->name : path, MAX_ENV);
                return -1;
            } else
                svc->envp[svc->env_count++] = strdup(val);
        }
    } else if (strcmp(line, "env_file") == 0) {
        while (*val == ' ' || *val == '\t') val++;
        if (val[*val == '-'] != '/' || strlen(val) >= sizeof svc->env_file[0]
            || svc->env_file_count >= MAX_ENV_FILES) {
            fprintf(stderr, "[schema-init] %s: env_file=%s must be an absolute path under %zu chars, at most %d per service\n",
                    svc->name[0] ? svc->name : path, val, sizeof svc->env_file[0], MAX_ENV_FILES);
            return -1;
        }
        strncpy(svc->env_file[svc->env_file_count++], val, sizeof svc->env_file[0] - 1);
    } else if (strcmp(line, "exec_pre") == 0) {
        const char *cmd = val + strspn(val, " \t");
        if (!svc_cmd_ok(cmd + strspn(cmd, "+-")) || svc->exec_pre_count >= MAX_EXEC_PRE) {
            fprintf(stderr, "[schema-init] %s: exec_pre=%s needs an absolute command with balanced quotes and at most 32 words, at most %d per service\n",
                    svc->name[0] ? svc->name : path, val, MAX_EXEC_PRE);
            return -1;
        }
        svc->exec_pre[svc->exec_pre_count++] = strdup(val + strspn(val, " \t"));
    } else if (strcmp(line, "exec_stop") == 0 || strcmp(line, "exec_stop_post") == 0) {
        int post = line[9] == '_';
        char **cmds = post ? svc->exec_stop_post : svc->exec_stop;
        int *n = post ? &svc->exec_stop_post_count : &svc->exec_stop_count;
        const char *cmd = val + strspn(val, " \t");
        if (!svc_cmd_ok(cmd + strspn(cmd, "-")) || *n >= MAX_EXEC_PRE) {
            fprintf(stderr, "[schema-init] %s: %s=%s needs an absolute command with balanced quotes and at most 32 words, at most %d per service\n",
                    svc->name[0] ? svc->name : path, line, val, MAX_EXEC_PRE);
            return -1;
        }
        cmds[(*n)++] = strdup(cmd);
    } else if (strcmp(line, "exec_reload") == 0) {
        const char *cmd = val + strspn(val, " \t");
        if (!svc_cmd_ok(cmd + strspn(cmd, "-")) || svc->exec_reload_count >= MAX_EXEC_PRE) {
            fprintf(stderr, "[schema-init] %s: exec_reload=%s needs an absolute command with balanced quotes and at most 32 words, at most %d per service\n",
                    svc->name[0] ? svc->name : path, val, MAX_EXEC_PRE);
            return -1;
        }
        svc->exec_reload[svc->exec_reload_count++] = strdup(cmd);
    } else if (strcmp(line, "reload_signal") == 0) {
        int sig = svc_reload_signal_parse(val);
        if (sig <= 0) {
            fprintf(stderr, "[schema-init] %s: reload_signal=%s is not a signal a daemon can catch (HUP, USR1, 10, ...)\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
        svc->reload_signal = sig;
        svc->reload_signal_set = 1;
    } else if (strcmp(line, "restart") == 0) {
        static const char *const pol[] = { "always", "no", "on-success", "on-failure",
                                           "on-abnormal", "on-abort", "on-watchdog" };
        int k = 0;
        while (k < 7 && strcmp(val, pol[k]) != 0) k++;
        if (k == 7) {
            fprintf(stderr, "[schema-init] %s: restart=%s is not always, no, on-success, on-failure, on-abnormal, on-abort or on-watchdog\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
        /* "no" lives only in SVC_NO_RESTART, so whatever lifts the flag (a
         * drop-in's no_restart=0, a fallback brought up) lifts it; the
         * last of restart= / no_restart= wins */
        svc->restart_policy = k == RESTART_NO ? RESTART_ALWAYS : (uint8_t)k;
        if (k == RESTART_NO) svc->flags |= SVC_NO_RESTART;
        else svc->flags &= ~SVC_NO_RESTART;
    } else if (strcmp(line, "success_exit_status") == 0 || strcmp(line, "restart_prevent_exit_status") == 0) {
        int succ = line[0] == 's';
        if (svc_status_list(val, succ ? svc->success_codes : svc->prevent_codes,
                            succ ? &svc->success_sigs : &svc->prevent_sigs) != 0) {
            fprintf(stderr, "[schema-init] %s: %s=%s takes exit codes 0-255 and signal names\n",
                    svc->name[0] ? svc->name : path, line, val);
            return -1;
        }
    } else if (strcmp(line, "kill_mode") == 0) {
        if (!strcmp(val, "control-group") || !strcmp(val, "cgroup")) svc->kill_mode = KILL_MODE_CGROUP;
        else if (!strcmp(val, "mixed")) svc->kill_mode = KILL_MODE_MIXED;
        else if (!strcmp(val, "process")) svc->kill_mode = KILL_MODE_PROCESS;
        else {
            fprintf(stderr, "[schema-init] %s: kill_mode=%s is not control-group, mixed or process\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
    } else if (strcmp(line, "kill_signal") == 0 || strcmp(line, "final_kill_signal") == 0) {
        int sig = svc_any_signal_parse(val);
        if (sig <= 0) {
            fprintf(stderr, "[schema-init] %s: %s=%s is not a signal (TERM, SIGINT, 9, ...)\n",
                    svc->name[0] ? svc->name : path, line, val);
            return -1;
        }
        if (line[0] == 'k') svc->kill_signal = sig;
        else svc->final_kill_signal = sig;
    } else if (strcmp(line, "reload") == 0) {
        /* the systemd1 shim's older opt-in: reload means SIGHUP */
        if (!svc->reload_signal_set) svc->reload_signal = atoi(val) == 1 || strcmp(val, "yes") == 0 ? SIGHUP : 0;
    } else if (strcmp(line, "card") == 0) {
        const char *c = val + strspn(val, " \t");
        int ok = strncmp(c, "exec:", 5) == 0 ? svc_cmd_ok(c + 5)
               : strncmp(c, "fallback:", 9) == 0 && c[9] && strlen(c + 9) < 64 && !strpbrk(c + 9, "/ \t");
        if (!ok || svc->card_count >= MAX_CARDS) {
            fprintf(stderr, "[schema-init] %s: card=%s needs exec: and an absolute command of at most 32 words, or fallback: and a service name; at most %d per service\n",
                    svc->name[0] ? svc->name : path, val, MAX_CARDS);
            return -1;
        }
        svc->card[svc->card_count++] = strdup(c);
    } else if (strcmp(line, "listen") == 0) {
        struct sockaddr_storage ss;
        socklen_t len;
        int type;
        while (*val == ' ' || *val == '\t') val++;
        if (svc->listen_count >= MAX_LISTEN || strlen(val) >= sizeof svc->listen[0]
            || service_listen_parse(val, &type, &ss, &len) < 0) {
            fprintf(stderr, "[schema-init] %s: bad listen=%s (stream|dgram|seqpacket|fifo: then /path, @abstract, PORT, ADDR:PORT or [ADDR]:PORT; at most %d)\n",
                    svc->name[0] ? svc->name : path, val, MAX_LISTEN);
            return -1;
        }
        snprintf(svc->listen[svc->listen_count++], sizeof svc->listen[0], "%s", val);
    } else if (strcmp(line, "condition") == 0) {
        const char *a;
        while (*val == ' ' || *val == '\t') val++;
        if (svc->cond_count >= MAX_COND || strlen(val) >= sizeof svc->cond[0] ||
            cond_kind(val, &a) < 0 || !a[*a == '|' ? (a[1] == '!' ? 2 : 1) : (*a == '!')]) {
            fprintf(stderr, "[schema-init] %s: bad condition=%s (KIND:[|][!]ARG, at most %d)\n",
                    svc->name[0] ? svc->name : path, val, MAX_COND);
            return -1;
        }
        snprintf(svc->cond[svc->cond_count++], sizeof svc->cond[0], "%s", val);
    } else if (strcmp(line, "wait_socket") == 0) {
        while (*val == ' ' || *val == '\t') val++;
        if (val[0] != '/' || strlen(val) >= sizeof svc->wait_socket) {
            fprintf(stderr, "[schema-init] %s: bad wait_socket=%s (absolute path, under %zu bytes)\n",
                    svc->name[0] ? svc->name : path, val, sizeof svc->wait_socket);
            return -1;
        }
        snprintf(svc->wait_socket, sizeof svc->wait_socket, "%s", val);
    } else if (strcmp(line, "listen_lazy") == 0) {
        svc->listen_lazy = svc->listen_hold = atoi(val) != 0;
    } else if (strcmp(line, "socket_mode") == 0) {
        char *end;
        long m = strtol(val, &end, 8);
        if (!*val || *end || m <= 0 || m > 07777) {
            fprintf(stderr, "[schema-init] %s: socket_mode=%s must be octal\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
        svc->socket_mode = (int)m;
    } else if (strcmp(line, "socket_user") == 0) {
        snprintf(svc->socket_user, sizeof svc->socket_user, "%s", val);
    } else if (strcmp(line, "socket_group") == 0) {
        snprintf(svc->socket_group, sizeof svc->socket_group, "%s", val);
    } else if (strcmp(line, "expand_args") == 0) {
        svc->expand_args = atoi(val) != 0;
    } else if (strcmp(line, "bind") == 0) {
        while (*val == ' ' || *val == '\t') val++;
        /* a drop-in replaces it, like other one-value keys */
        if (svc->bind_name[0] && !pc->dropin) {
            fprintf(stderr, "[schema-init] %s: more than one bind= line\n", svc->name[0] ? svc->name : path);
            return -1;
        }
        if (!*val || strlen(val) >= sizeof svc->bind_name) {
            fprintf(stderr, "[schema-init] %s: bad bind=%s\n", svc->name[0] ? svc->name : path, val);
            return -1;
        }
        snprintf(svc->bind_name, sizeof svc->bind_name, "%s", val);
    } else if (strcmp(line, "dep") == 0) {
        /* bind= adds its dep at finalize; a dep= naming it too isn't a second slot */
        for (int k = 0; k < pc->dep_slot; k++)
            if (!strcmp(svc->dep_name[k], val)) return 0;
        if (pc->dep_slot >= MAX_DEPS) {
            fprintf(stderr, "[schema-init] %s: more than %d dep= lines\n",
                    svc->name[0] ? svc->name : path, MAX_DEPS);
            return -1;
        }
        strncpy(svc->dep_name[pc->dep_slot++], val, 63);
    } else if (strcmp(line, "after") == 0 || strcmp(line, "before") == 0) {
        char (*names)[64] = line[0] == 'a' ? svc->after_name : svc->before_name;
        int k;
        size_t n;
        while (*val == ' ' || *val == '\t') val++;
        n = strlen(val);
        while (n && (val[n - 1] == ' ' || val[n - 1] == '\t')) val[--n] = '\0';
        /* one name per line: "after=a b" naming no service would order nothing, silently */
        if (!*val || n >= sizeof names[0] || strpbrk(val, " \t")) {
            fprintf(stderr, "[schema-init] %s: bad %s=%s\n", svc->name[0] ? svc->name : path, line, val);
            return -1;
        }
        for (k = 0; k < MAX_ORDER && names[k][0]; k++)
            if (!strcmp(names[k], val)) return 0;
        if (k >= MAX_ORDER) {
            fprintf(stderr, "[schema-init] %s: more than %d %s= lines\n",
                    svc->name[0] ? svc->name : path, MAX_ORDER, line);
            return -1;
        }
        snprintf(names[k], sizeof names[k], "%s", val);
    } else if (strcmp(line, "oneshot") == 0 && atoi(val))
        svc->flags |= SVC_ONESHOT;
    else if (strcmp(line, "needs_root") == 0 && atoi(val))
        svc->flags |= SVC_NEEDS_ROOT;
    else if (strcmp(line, "no_new_privs") == 0) {
        svc->hard_set |= HARD_NNP;
        if (atoi(val)) svc->flags |= SVC_NO_NEW_PRIVS;
    }
    else if (strcmp(line, "keep_caps") == 0) {
        if (parse_cap_list(val, &svc->cap_keep_mask) != 0) {
            fprintf(stderr, "[schema-init] %s: unknown capability in keep_caps=%s\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
        svc->cap_restrict = 1;
    }
    else if (strcmp(line, "ambient_caps") == 0) {
        if (parse_cap_list(val, &svc->cap_ambient_mask) != 0) {
            fprintf(stderr, "[schema-init] %s: unknown capability in ambient_caps=%s\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
    }
    else if ((nsr = parse_ns_field(svc, line, val)) != 0) {
        if (nsr < 0) return -1;
    }
    else if (strcmp(line, "landlock_ro") == 0 || strcmp(line, "landlock_rw") == 0) {
        while (*val == ' ' || *val == '\t') val++;
        if (val[0] != '/' || strpbrk(val, " \t") || svc->landlock_count >= MAX_LANDLOCK) {
            fprintf(stderr, "[schema-init] %s: bad %s=%s (one absolute path without whitespace, at most %d)\n",
                    svc->name[0] ? svc->name : path, line, val, MAX_LANDLOCK);
            return -1;
        }
        if (line[10] == 'w') svc->landlock_rw |= 1U << svc->landlock_count;
        svc->landlock[svc->landlock_count++] = strdup(val);
    }
    else if (strcmp(line, "rw_path") == 0) {
        while (*val == ' ' || *val == '\t') val++;
        if (!rw_path_ok(val) || svc->rw_path_count >= MAX_RW_PATHS) {
            fprintf(stderr, "[schema-init] %s: bad rw_path=%s (absolute, no '..', at most %d)\n",
                    svc->name[0] ? svc->name : path, val, MAX_RW_PATHS);
            return -1;
        }
        svc->rw_path[svc->rw_path_count++] = strdup(val);
    }
    else if (exec_dir_kind(line, 0) >= 0) {
        int k = exec_dir_kind(line, 0);
        char buf[1024], *tok, *save = NULL;
        if (strlen(val) >= sizeof buf) {
            fprintf(stderr, "[schema-init] %s: %s= is too long\n", svc->name[0] ? svc->name : path, line);
            return -1;
        }
        strcpy(buf, val);
        for (tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
            int dup = 0;
            if (!exec_dir_ok(tok)) {
                fprintf(stderr, "[schema-init] %s: %s=%s must be relative, without '.', '..', '//' or ':'\n",
                        svc->name[0] ? svc->name : path, line, tok);
                return -1;
            }
            for (int i = 0; i < svc->exec_dir_count[k]; i++) dup |= strcmp(svc->exec_dir[k][i], tok) == 0;
            if (dup) continue;
            if (svc->exec_dir_count[k] >= MAX_EXEC_DIRS) {
                fprintf(stderr, "[schema-init] %s: %s= takes at most %d directories\n",
                        svc->name[0] ? svc->name : path, line, MAX_EXEC_DIRS);
                return -1;
            }
            svc->exec_dir[k][svc->exec_dir_count[k]++] = strdup(tok);
        }
    }
    else if (strcmp(line, "dir_owner") == 0) {
        /* a truncated name could resolve to some other user: refuse the load */
        if (strlen(val) >= sizeof svc->dir_owner || *val == ':' || strchr(val, ':') != strrchr(val, ':')) {
            fprintf(stderr, "[schema-init] %s: dir_owner=%s is not USER[:GROUP] (under %zu chars)\n",
                    svc->name[0] ? svc->name : path, val, sizeof svc->dir_owner);
            return -1;
        }
        memset(svc->dir_owner, 0, sizeof svc->dir_owner);
        strcpy(svc->dir_owner, val);
    }
    else if (exec_dir_kind(line, 1) >= 0) {
        char *end;
        unsigned long m = strtoul(val, &end, 8);
        if (*val && (*end || m > 07777)) {
            fprintf(stderr, "[schema-init] %s: %s=%s is not an octal mode\n",
                    svc->name[0] ? svc->name : path, line, val);
            return -1;
        }
        int k = exec_dir_kind(line, 1);
        svc->exec_dir_mode[k] = (uint16_t)(*val ? m : 0);
        if (*val) svc->exec_dir_mode_set |= 1 << k;
        else svc->exec_dir_mode_set &= ~(1 << k);
    }
    else if (strcmp(line, "landlock_bind_tcp") == 0 || strcmp(line, "landlock_connect_tcp") == 0) {
        int k = line[9] == 'c', ntok = 0, none = 0, bad = strlen(val) >= 256;
        char *tok, *save = NULL, buf[256] = "";
        if (!bad) memcpy(buf, val, strlen(val) + 1);
        if (svc->landlock_tcp_n[k] < 0) svc->landlock_tcp_n[k] = 0;
        for (tok = strtok_r(buf, ", \t", &save); tok && !bad; tok = strtok_r(NULL, ", \t", &save)) {
            char *end;
            long port = strtol(tok, &end, 10);
            ntok++;
            if (!strcmp(tok, "none")) { none = 1; continue; }
            bad = !isdigit((unsigned char)*tok) || *end || port > 65535 || svc->landlock_tcp_n[k] >= MAX_LANDLOCK;
            if (!bad) svc->landlock_tcp[k][svc->landlock_tcp_n[k]++] = (uint16_t)port;
        }
        if (bad || !ntok || (none && ntok > 1)) {
            fprintf(stderr, "[schema-init] %s: bad %s=%s (TCP ports 0-65535, at most %d, or none alone)\n",
                    svc->name[0] ? svc->name : path, line, val, MAX_LANDLOCK);
            return -1;
        }
        if (none) svc->landlock_tcp_n[k] = 0;
    }
    else if (strcmp(line, "critical") == 0 && atoi(val))
        svc->flags |= SVC_CRITICAL;
    else if (strcmp(line, "no_restart") == 0 && atoi(val))
        svc->flags |= SVC_NO_RESTART;
    else if (strcmp(line, "stop_first") == 0 && atoi(val))
        svc->flags |= SVC_STOP_FIRST;
    else if (strcmp(line, "main_pid_notify") == 0 && atoi(val))
        svc->flags |= SVC_MAIN_PID_NOTIFY;
    else if (strcmp(line, "stable_secs") == 0 && (atoi(val) > 0 || strcmp(val, "0") == 0))
        svc->stable_secs = atoi(val);
    else if (strcmp(line, "oom_score_adj") == 0) {
        int v = atoi(val);
        if (v >= -1000 && v <= 1000) { svc->oom_score_adj = v; svc->oom_adj_set = 1; }
    }
    else if (strcmp(line, "ready_bus_name") == 0)
        snprintf(svc->ready_bus_name, sizeof svc->ready_bus_name, "%s", val);
    else if (strcmp(line, "notify") == 0)
        svc->notify = atoi(val) ? 1 : 0;
    else if (strcmp(line, "fdstore") == 0) {
        int v = atoi(val);
        if (v >= 0 && v <= 4096) svc->fdstore_max = v;
    }
    else if (strcmp(line, "ready_path") == 0)
        strncpy(svc->ready_path, val, sizeof(svc->ready_path) - 1);
    else if (strcmp(line, "working_dir") == 0) {
        const char *d = val + (*val == '-');
        if ((d[0] != '/' && strcmp(d, "~") != 0) || strlen(val) >= sizeof svc->working_dir) {
            fprintf(stderr, "[schema-init] %s: working_dir=%s needs an absolute path or ~, optionally after -\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
        strcpy(svc->working_dir, val);
    }
    else if (strncmp(line, "verify_", 7) == 0 &&
             (!strcmp(line + 7, "memory_max") || !strcmp(line + 7, "cpu_max") || !strcmp(line + 7, "pids_max"))) {
        char *dst = line[7] == 'm' ? svc->verify_memory_max : line[7] == 'c' ? svc->verify_cpu_max : svc->verify_pids_max;
        size_t dn = line[7] == 'm' ? sizeof svc->verify_memory_max : line[7] == 'c' ? sizeof svc->verify_cpu_max
                                                                                : sizeof svc->verify_pids_max;
        if (strlen(val) >= dn || strspn(val, "0123456789 ") != strlen(val)) {
            fprintf(stderr, "[schema-init] %s: %s=%s must be the cgroup file's numbers\n",
                    svc->name[0] ? svc->name : path, line, val);
            return -1;
        }
        snprintf(dst, dn, "%s", val);
    }
    else if (strcmp(line, "pid_file") == 0)
        snprintf(svc->pid_file, sizeof svc->pid_file, "%s", val);
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
    } else if (strcmp(line, "watchdog_sec") == 0) {
        if (*val && strcmp(val, "0") && cg_key_int(svc, path, line, val, 86400, "0-86400 seconds") < 0)
            return -1;
        svc->watchdog_sec = atoi(val);
    } else if (strcmp(line, "cpu_limit") == 0) {
        /* atoi let "abc" or "20000" through as 0: no limit at all */
        if (*val && strcmp(val, "0") && cg_key_int(svc, path, line, val, 10000, "1-10000 (% of one core), 0 or empty") < 0)
            return -1;
        svc->cpu_limit_pct = atoi(val);
    } else if (strcmp(line, "pids_max") == 0) {
        /* empty clears, so a drop-in can lift a limit the base file set */
        int unlimited = !*val || !strcmp(val, "max") || !strcmp(val, "infinity");
        if (!unlimited && cg_key_int(svc, path, line, val, 4194304, "1-4194304, max or empty") < 0)
            return -1;
        svc->pids_max = unlimited ? 0 : atoi(val);
    } else if (strcmp(line, "cpu_weight") == 0) {
        if (*val && cg_key_int(svc, path, line, val, 10000, "1-10000, or empty for the priority tier") < 0)
            return -1;
        svc->cpu_weight = *val ? atoi(val) : 0;
    } else if (strcmp(line, "mem_limit") == 0) {
        /* MB; atol read "1G" as 1 MB and the service died of OOM at start */
        if (mem_limit_parse(val, &svc->mem_limit_mb) < 0) {
            fprintf(stderr, "[schema-init] %s: mem_limit=%s must be megabytes (512, 512M) or gigabytes (2G), 0 or empty\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
    } else if (strcmp(line, "mem_low") == 0) {
        if (mem_limit_parse(val, &svc->mem_low_mb) < 0) {
            fprintf(stderr, "[schema-init] %s: mem_low=%s must be megabytes (512, 512M) or gigabytes (2G), 0 or empty\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
    } else if (strcmp(line, "swap_max") == 0) {
        svc->swap_max_set = *val != '\0';
        svc->swap_max_mb = 0;
        if (strcasecmp(val, "max") == 0 || strcasecmp(val, "infinity") == 0) {
            svc->swap_max_mb = -1;
        } else if (mem_limit_parse(val, &svc->swap_max_mb) < 0) {
            fprintf(stderr, "[schema-init] %s: swap_max=%s must be megabytes (512, 512M), gigabytes (2G), 0, max or empty\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
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
    } else if (strcmp(line, "restart_sec") == 0) {
        int v = atoi(val);
        svc->restart_sec = v < 0 ? 0 : v > 3600 ? 3600 : v;
    } else if (strcmp(line, "start_timeout_sec") == 0) {
        svc->start_timeout_sec = atoi(val);
    } else if (strcmp(line, "nice") == 0 || strcmp(line, "io_class") == 0 ||
               strcmp(line, "io_priority") == 0 || strcmp(line, "sched_policy") == 0 ||
               strcmp(line, "sched_priority") == 0) {
        if (parse_sched_key(svc, line, val) != 0) {
            fprintf(stderr, "[schema-init] %s: bad %s=%s\n", svc->name[0] ? svc->name : path, line, val);
            return -1;
        }
    } else if (strcmp(line, "syscall_filter") == 0) {
        /* the first line sets the mode (a leading ~ = denylist); a later
         * line of the opposite sign takes names back out (systemd's rules) */
        char buf[512], *save, *t;
        if (!*val) {
            memset(&svc->sc.set, 0, sizeof svc->sc.set);
            memset(&svc->sc.err_set, 0, sizeof svc->sc.err_set);
            svc->sc.n_err = 0;
            svc->sc.mode = SC_NONE;
            return 0;
        }
        int neg = val[0] == '~';
        snprintf(buf, sizeof buf, "%s", val + neg);
        if (svc->sc.mode == SC_NONE) svc->sc.mode = neg ? SC_DENY : SC_ALLOW;
        int remove = (svc->sc.mode == SC_ALLOW) == neg;
        for (t = strtok_r(buf, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
            int r = sc_add(&svc->sc, t, remove);
            if (r == -2) {
                fprintf(stderr, "[schema-init] %s: syscall_filter: unknown syscall '%s' ignored\n",
                        svc->name[0] ? svc->name : path, t);
            } else if (r != 0) {
                fprintf(stderr, "[schema-init] %s: syscall_filter: %s '%s' — not loaded\n", svc->name[0] ? svc->name : path,
                        r == -3 ? "more than 4 distinct errnos at" : "unknown group or errno", t);
                return -1;
            }
        }
    } else if (strcmp(line, "syscall_deny") == 0) {
        /* resolved at finalize, where the host default is known */
        if (strlen(val) >= sizeof svc->sc_deny) {
            fprintf(stderr, "[schema-init] %s: syscall_deny too long (limit %zu) — not loaded\n",
                    svc->name[0] ? svc->name : path, sizeof svc->sc_deny - 1);
            return -1;
        }
        snprintf(svc->sc_deny, sizeof svc->sc_deny, "%s", val);
        if (*val) svc->hard_set |= HARD_SD;
        else svc->hard_set &= ~HARD_SD;
    } else if (strcmp(line, "syscall_error_number") == 0) {
        if (!*val) svc->sc.err = 0;
        else if (sc_errno_parse(val, &svc->sc.err) != 0) {
            fprintf(stderr, "[schema-init] %s: bad syscall_error_number=%s — not loaded\n", svc->name[0] ? svc->name : path, val);
            return -1;
        }
    } else if (strcmp(line, "syscall_architectures") == 0) {
        if (*val && strcmp(val, "native") != 0) {
            fprintf(stderr, "[schema-init] %s: syscall_architectures=%s: only 'native' — not loaded\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
        svc->sc.native = *val != 0;
    } else if (strcmp(line, "restrict_suid_sgid") == 0 || strcmp(line, "lock_personality") == 0 ||
               strcmp(line, "restrict_realtime") == 0 || strcmp(line, "restrict_namespaces") == 0) {
        long n;
        if (parse_int_in(val, 0, 1, &n)) {
            fprintf(stderr, "[schema-init] %s: bad %s=%s (0 or 1) — not loaded\n", svc->name[0] ? svc->name : path, line, val);
            return -1;
        }
        uint8_t *f = !strcmp(line, "restrict_suid_sgid") ? &svc->sc.suid
                   : !strcmp(line, "lock_personality") ? &svc->sc.persona
                   : !strcmp(line, "restrict_realtime") ? &svc->sc.rt : &svc->sc.ns;
        *f = (uint8_t)n;
    } else if (strcmp(line, "supplementary_groups") == 0) {
        if (!*val) svc->supp_count = 0;   /* a drop-in's empty value clears */
        for (char *tok = val, *sp; *tok; tok = sp) {
            while (*tok == ' ' || *tok == '\t') tok++;
            if (!*tok) break;
            sp = tok + strcspn(tok, " \t");
            size_t n = (size_t)(sp - tok);
            if (svc->supp_count >= 16 || n >= sizeof svc->supp_groups[0] || strspn(tok, "abcdefghijklmnopqrstuvwxyz"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") < n) {
                fprintf(stderr, "[schema-init] %s: bad supplementary_groups=%s (at most 16 names or gids) — not loaded\n",
                        svc->name[0] ? svc->name : path, val);
                return -1;
            }
            memcpy(svc->supp_groups[svc->supp_count], tok, n);
            svc->supp_groups[svc->supp_count++][n] = '\0';
        }
    } else if (strcmp(line, "dynamic_user") == 0) {
        long n;
        if (parse_int_in(val, 0, 1, &n)) {
            fprintf(stderr, "[schema-init] %s: bad dynamic_user=%s — not loaded\n", svc->name[0] ? svc->name : path, val);
            return -1;
        }
        svc->dynamic_user = (uint8_t)n;
    } else if (strcmp(line, "watch_path") == 0 || strcmp(line, "make_directory") == 0 ||
               strcmp(line, "directory_mode") == 0 || strcmp(line, "trigger_limit_burst") == 0 ||
               strcmp(line, "trigger_limit_interval_sec") == 0) {
        if (parse_watch_key(svc, line, val) != 0) {
            fprintf(stderr, "[schema-init] %s: bad %s=%s%s\n", svc->name[0] ? svc->name : path, line, val,
                    !strcmp(line, "watch_path") && svc->watch_count >= MAX_WATCH ? " (at most 8)" : "");
            return -1;
        }
    } else if (strcmp(line, "umask") == 0 && !*val) {
        svc->umask_val = -1;
    } else if (strcmp(line, "umask") == 0) {
        size_t n = strspn(val, "01234567");
        long v = n && n <= 4 && !val[n] ? strtol(val, NULL, 8) : -1;
        if (v < 0 || v > 0777) {
            fprintf(stderr, "[schema-init] %s: bad umask=%s (octal, at most 0777)\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
        svc->umask_val = (int16_t)v;
    } else if (strcmp(line, "stop_timeout_sec") == 0) {
        int v = atoi(val);
        svc->stop_timeout_sec = (v >= 1 && v <= 300) ? v : 0;
    } else if (strcmp(line, "on_boot_sec") == 0) {
        svc->timer_boot_sec = atoi(val);
        svc->flags |= SVC_TIMER | SVC_ONESHOT;
    } else if (strcmp(line, "on_active_sec") == 0) {
        svc->timer_interval_sec = atoi(val);
        svc->flags |= SVC_TIMER | SVC_ONESHOT;
    } else if (strcmp(line, "on_calendar") == 0) {
        /* ignored, the line left a plain daemon that ran at boot and crash-looped */
        if (*val && parse_calendar(svc, pc, val) != 0) {
            fprintf(stderr, "[schema-init] %s: bad on_calendar='%s' (want [DOW] [DOM] H:M, "
                    "at most 4 lines, one that can occur) — not loaded\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
    } else if (strcmp(line, "randomized_delay_sec") == 0) {
        long n;
        if (parse_int_in(val, 0, 2678400, &n)) {
            fprintf(stderr, "[schema-init] %s: bad randomized_delay_sec=%s (0..2678400) — not loaded\n",
                    svc->name[0] ? svc->name : path, val);
            return -1;
        }
        svc->timer_jitter_sec = (int)n;
    } else if (strcmp(line, "persistent") == 0) {
        if (atoi(val)) svc->flags |= SVC_TIMER_PERSIST;
    } else if (strcmp(line, "user") == 0) {
        char *end;
        errno = 0;
        unsigned long id = strtoul(val, &end, 10);
        int num = *val && !*end;
        struct passwd *pw = !num ? getpwnam(val)
                          : !errno && id < (uid_t)-1 ? getpwuid((uid_t)id) : NULL;
        svc->run_uid = pw ? pw->pw_uid : 0;
        svc->run_gid = pw ? pw->pw_gid : 0;
        memset(svc->run_user, 0, sizeof svc->run_user);
        strncpy(svc->run_user, pw ? pw->pw_name : val, sizeof(svc->run_user) - 1);
        if (!pw && *val)
            fprintf(stderr, "[schema-init] %s: user=%s does not exist yet — "
                    "it will be looked up again at start\n", path, val);
    } else if (strcmp(line, "group") == 0) {
        /* a truncated name could resolve to SOME OTHER group: refuse the load */
        if (strlen(val) >= sizeof svc->run_group) {
            fprintf(stderr, "[schema-init] %s: group=%s longer than %zu chars — service not loaded\n",
                    path, val, sizeof svc->run_group - 1);
            return -1;
        }
        /* stored by name and resolved at start, after user=: the gid must not
         * depend on which line comes first in the file */
        memset(svc->run_group, 0, sizeof svc->run_group);
        strncpy(svc->run_group, val, sizeof(svc->run_group) - 1);
        if (*val && !svc_group_resolve(val, NULL))
            fprintf(stderr, "[schema-init] %s: group=%s does not exist yet — "
                    "it will be looked up again at start\n", path, val);
    }
    return 0;
}

int svc_refused;

int service_open_trusted(const char *path) {
    struct stat st;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 022)) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    return fd;
}

static int svc_parse_file(service_t *svc, struct parse_ctx *pc, const char *path) {
    char line[512];
    pc->cal_file = 0;
    int fd = service_open_trusted(path);
    if (fd < 0) {
        if (errno == EPERM) {
            fprintf(stderr, "[schema-init] REFUSED %s: not a regular file owned by root and "
                    "writable only by its owner — service not loaded (chown root:root; chmod go-w)\n", path);
            svc_refused++;
        }
        return -1;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return -1; }
    int r;
    while ((r = svc_read_line(f, line, sizeof(line))) != 0) {
        if (r < 0) {
            fprintf(stderr, "[schema-init] %s: line longer than %zu bytes — service not loaded\n",
                    path, sizeof(line) - 1);
            fclose(f);
            return -1;
        }
        if (svc_parse_line(svc, pc, line, path) != 0) {
            if (pc->dropin)
                fprintf(stderr, "[schema-init] bad line in drop-in %s — service not loaded\n", path);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

/* the name is a cgroup dir, a log file name and a word on the control socket */
static int svc_name_ok(const char *n) {
    if (n[0] == '.') return 0;
    for (; *n; n++)
        if (!isalnum((unsigned char)*n) && !strchr("._@:+-\\", *n)) return 0;
    return 1;
}

/* Post-parse checks and defaults, then the name from the filename.
 * 0 = loadable, -1 = rejected (template file, no name, no exec, bad hardening). */
/* syscall_deny=: the schema-native denylist (the 10-13 plan), merged into
 * the one filter: a denylist grows, an allowlist loses those names. Opt-in
 * only, NOT under the hardening-default: a knob added to that switch lands
 * on every host .svc written before it existed (blakbox 11-18: mount-home
 * lost mount(2), /home never mounted). */
static const char *const sc_deny_preset =
    "@clock @cpu-emulation @debug @module @mount @obsolete @raw-io @reboot @swap kexec_load kexec_file_load bpf";

static int sc_deny_finalize(service_t *svc) {
    const char *v = svc->sc_deny;
    char buf[512], *save, *t;
    sc_spec_t d = { .mode = SC_DENY };
    if (!(svc->hard_set & HARD_SD)) return 0;
    if (!strcmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcasecmp(v, "false") ||
        !strcasecmp(v, "none"))
        return 0;
    int preset = !strcasecmp(v, "default") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcasecmp(v, "true");
    snprintf(buf, sizeof buf, "%s", preset ? sc_deny_preset : v);
    for (t = strtok_r(buf, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
        /* ~ is syscall_filter's; "default" only stands alone; a bare number is a mistake */
        int r = *t == '~' || !strcasecmp(t, "default") || !t[strspn(t, "0123456789")] ? -1 : sc_add(&d, t, 0);
        if (r == -2) {
            fprintf(stderr, "[schema-init] %s: syscall_deny: unknown syscall '%s' ignored\n", svc->name, t);
        } else if (r != 0) {
            fprintf(stderr, "[schema-init] %s: syscall_deny: bad entry '%s' (0, default, or names/@groups) — not loaded\n",
                    svc->name, t);
            return -1;
        }
    }
    /* an all-unknown list must not install an empty filter: it would still refuse foreign ABIs */
    int any = 0;
    for (int w = 0; w < SC_WORDS; w++) any |= d.set[w] != 0;
    if (!any) {
        fprintf(stderr, "[schema-init] %s: syscall_deny: no known syscalls — nothing denied\n", svc->name);
        return 0;
    }
    if (svc->sc.mode == SC_ALLOW && d.n_err)
        fprintf(stderr, "[schema-init] %s: syscall_deny: per-entry errno ignored under an allowlist syscall_filter=\n",
                svc->name);
    /* one filter: a name the unit's syscall_filter= already lists keeps its errno */
    if (svc->sc.mode == SC_NONE) {
        svc->sc.mode = SC_DENY;
        memcpy(svc->sc.set, d.set, sizeof d.set);
        for (int k = 0; k < d.n_err; k++) {
            svc->sc.err_val[k] = d.err_val[k];
            memcpy(svc->sc.err_set[k], d.err_set[k], sizeof d.err_set[k]);
        }
        svc->sc.n_err = d.n_err;
    } else if (svc->sc.mode == SC_ALLOW) {
        for (int w = 0; w < SC_WORDS; w++) svc->sc.set[w] &= ~d.set[w];
    } else {
        for (int w = 0; w < SC_WORDS; w++) {
            uint64_t add = d.set[w] & ~svc->sc.set[w];
            svc->sc.set[w] |= add;
            for (int k = 0; k < d.n_err; k++) {
                if (!(d.err_set[k][w] & add)) continue;
                int e;
                for (e = 0; e < svc->sc.n_err && svc->sc.err_val[e] != d.err_val[k]; e++) {}
                if (e == svc->sc.n_err) {
                    if (e >= SC_MAX_ERRS) {
                        fprintf(stderr, "[schema-init] %s: syscall_deny: too many errnos with syscall_filter= — not loaded\n",
                                svc->name);
                        return -1;
                    }
                    svc->sc.err_val[svc->sc.n_err++] = d.err_val[k];
                }
                svc->sc.err_set[e][w] |= d.err_set[k][w] & add;
            }
        }
    }
    return 0;
}

int (*service_relexec_keep)(const char *name);

static int svc_finalize(service_t *svc, struct parse_ctx *pc, const char *path) {
    if (hardening_finalize(svc) != 0) return -1;
    if (sc_deny_finalize(svc) != 0) return -1;
    /* drop-ins applied: what start/restart/reset put back after a stop's hold */
    svc->no_restart_cfg = (svc->flags & SVC_NO_RESTART) != 0;
    /* bind=X is also dep=X (start order, shutdown order, cycles): after the
     * drop-ins, so an empty dep= in one can't drop it */
    if (svc->bind_name[0] && (svc->flags & SVC_TIMER)) {
        /* a held timer would come back with a stale timer_next and fire at once */
        fprintf(stderr, "[schema-init] %s: bind= on a timer — not loaded\n", svc->name);
        return -1;
    }
    if (svc->bind_name[0]) {
        int k, have = 0;
        for (k = 0; k < pc->dep_slot; k++) have |= !strcmp(svc->dep_name[k], svc->bind_name);
        if (!have) {
            if (pc->dep_slot >= MAX_DEPS) {
                fprintf(stderr, "[schema-init] %s: bind=%s needs a dep slot, %d dep= lines already\n",
                        svc->name, svc->bind_name, MAX_DEPS);
                return -1;
            }
            snprintf(svc->dep_name[pc->dep_slot++], sizeof svc->dep_name[0], "%s", svc->bind_name);
        }
    }
    if (svc->watch_count && (svc->listen_count || (svc->flags & SVC_TIMER))) {
        fprintf(stderr, "[schema-init] %s: watch_path= cannot be combined with listen= or timers — not loaded\n",
                svc->name);
        return -1;
    }
    for (int k = EXEC_DIR_STATE; svc->dynamic_user && k < EXEC_DIR_KINDS; k++)
        for (int i = 0; i < svc->exec_dir_count[k]; i++)
            if (strchr(svc->exec_dir[k][i], '/')) {
                /* <base>/<d> is a symlink into private/: one level only */
                fprintf(stderr, "[schema-init] %s: dynamic_user=1 takes no nested state/cache/logs dir (%s) — not loaded\n",
                        svc->name, svc->exec_dir[k][i]);
                return -1;
            }
    if (svc->watch_count && svc->pid_file[0]) {
        /* the launcher's exit would end the run while its daemon lives on */
        fprintf(stderr, "[schema-init] %s: watch_path= cannot be combined with pid_file= — not loaded\n",
                svc->name);
        return -1;
    }
    if (svc->watch_count) svc->flags |= SVC_PATH;
    if (service_verifies_limits(svc) && !svc->notify) {
        fprintf(stderr, "[schema-init] %s: verify_*_max needs notify=1 (checked on READY=1) — not loaded\n", svc->name);
        return -1;
    }
    if ((svc->flags & SVC_MAIN_PID_NOTIFY) && !svc->notify) {
        fprintf(stderr, "[schema-init] %s: main_pid_notify=1 needs notify=1 — not loaded\n", svc->name);
        return -1;
    }
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

    /* default start timeout: protect oneshots (but not timers, which may
     * legitimately run long); daemons rely on stable_secs instead */
    if (svc->start_timeout_sec == -1) {
        svc->start_timeout_sec =
            ((svc->flags & SVC_ONESHOT) && !(svc->flags & SVC_TIMER))
            ? ONESHOT_START_TIMEOUT : 0;
    }

    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;
    size_t flen = strlen(fname);
    if (flen >= 5 && strcmp(fname + flen - 4, ".svc") == 0) {
        char base_name[256];
        size_t blen = flen - 4;
        if (blen >= sizeof(base_name)) blen = sizeof(base_name) - 1;
        memcpy(base_name, fname, blen);
        base_name[blen] = '\0';
        char *bat = strchr(base_name, '@');
        /* template file itself (motor@.svc) — not a spawnable instance */
        if (bat && !*(bat + 1)) return -1;
        if (bat || !svc->name[0]) {
            strncpy(svc->name, base_name, sizeof(svc->name) - 1);
            svc->name[sizeof(svc->name) - 1] = '\0';
        }
        if (bat)
            strncpy(svc->instance, bat + 1, sizeof(svc->instance) - 1);
    }

    if (!svc->name[0] || !svc->exec[0]) return -1;
    if (!svc_name_ok(svc->name)) {
        fprintf(stderr, "[schema-init] %s: name '%s' may hold only A-Z a-z 0-9 . _ @ : + - \\ "
                "and not start with '.' — not loaded\n", path, svc->name);
        return -1;
    }
    /* execv, no PATH search: a relative exec= runs whatever sits at that path under
     * working_dir= (or /). Last, once the name is final (name= may follow exec=, or
     * come from the file or instance name). One already loaded across a re-exec or
     * reload is kept until the next boot, so an upgrade never drops a live service */
    if (svc->exec[0] != '/') {
        if (!service_relexec_keep || !service_relexec_keep(svc->name)) {
            fprintf(stderr, "[schema-init] %s: exec=%s is not an absolute path — not loaded\n",
                    svc->name, svc->exec);
            return -1;
        }
        fprintf(stderr, "[schema-init] %s: exec=%s is not an absolute path; kept until the "
                "next boot, which refuses it\n", svc->name, svc->exec);
    }
    svc->argv[pc->argc] = NULL;
    return 0;
}

static int svc_load_file(const char *path, service_t *svc) {
    struct parse_ctx pc = { 1, 0, 0, 0 };
    /* `systemctl mask` leaves an empty <name>.svc.masked twin; here, so that
     * schema-ctl add can't load it either */
    char masked[SVC_DROPIN_PATH + 8];
    snprintf(masked, sizeof masked, "%s.masked", path);
    if (access(masked, F_OK) == 0) {
        fprintf(stderr, "[schema-init] %s: masked, not loaded\n", path);
        return -1;
    }
    char dropins[SVC_DROPIN_MAX][SVC_DROPIN_PATH];
    int nd = svc_dropin_list(path, dropins, SVC_DROPIN_MAX);
    if (nd < 0) {
        fprintf(stderr, "[schema-init] REFUSED %s: a drop-in directory is not owned by root or is "
                "writable by others — service not loaded (chown root:root; chmod go-w)\n", path);
        svc_refused++;
        return -1;
    }
    svc_init_defaults(svc);
    if (svc_parse_file(svc, &pc, path) != 0) goto bad;
    pc.dropin = 1;
    for (int i = 0; i < nd; i++)
        if (svc_parse_file(svc, &pc, dropins[i]) != 0) goto bad;
    if (svc_finalize(svc, &pc, path) != 0) goto bad;
    svc->content_hash = svc_content_hash(path, dropins, nd);
    return 0;
bad:
    svc_free_strings(svc);
    return -1;
}

static int svc_dir_chain_trusted(const char *dir) {
    char p[PATH_MAX];
    if (!realpath(dir, p)) return 0;
    for (int top = 1;; top = 0) {
        struct stat st;
        char *s;
        if (stat(p, &st) < 0 || !svc_dir_trusted(&st, !top)) return 0;
        s = strrchr(p, '/');
        if (s != p) *s = '\0';
        else if (p[1]) p[1] = '\0';
        else return 1;
    }
}

int services_load(const char *dir, service_t *table, int max) {
    DIR *d = opendir(dir);
    svc_refused = 0;
    if (d && !svc_dir_chain_trusted(dir)) {
        fprintf(stderr, "[schema-init] WARN: %s or a parent directory is not owned by root or is "
                "writable by others — loading anyway; schema-ctl reload is refused until fixed (chown root:root; chmod go-w)\n", dir);
        svc_refused++;
    }
    struct dirent **ents;
    int count = 0;
    int warned = 0;

    if (!d) return 0;
    closedir(d);
    /* sorted: readdir order made start order and table order differ per
     * filesystem, and decided which of two same-named files won */
    int nents = scandir(dir, &ents, NULL, svc_dropin_cmp);
    if (nents < 0) return 0;

    for (int e = 0; e < nents; e++) {
        char path[512];
        const char *dn = ents[e]->d_name;
        size_t nlen = strlen(dn);

        /* only .svc files */
        if (nlen < 5 || strcmp(dn + nlen - 4, ".svc") != 0)
            continue;

        if (count >= max) {
            if (!warned) {
                fprintf(stderr, "schema-init: WARNING — MAX_SERVICES (%d) reached; extra .svc files ignored\n", max);
                warned = 1;
            }
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", dir, dn);
        if (svc_load_file(path, &table[count]) != 0)
            continue;
        /* two files with one name= would share a cgroup and a log, and every
         * lookup by name would find only the first */
        int k;
        for (k = 0; k < count && strcmp(table[k].name, table[count].name); k++)
            ;
        if (k < count) {
            fprintf(stderr, "[schema-init] REFUSED %s: name '%s' is already taken by an earlier "
                    ".svc — not loaded\n", path, table[count].name);
            svc_free_strings(&table[count]);
            continue;
        }
        count++;
    }
    for (int e = 0; e < nents; e++) free(ents[e]);
    free(ents);

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
    return svc_load_file(path, svc);
}


/* ── ordering: after= / before= ──────────────────────────────────────── */

static int svc_find(const service_t *table, int count, const char *name) {
    for (int j = 0; j < count; j++)
        if (!strcmp(table[j].name, name)) return j;
    return -1;
}

typedef struct { const group_t *g; int n; } order_grps_t;

/* does from reach to over dep= (a group dep through its members) and the order
 * edges kept so far */
static int order_reaches(const service_t *t, int count, const order_grps_t *gr,
                         int from, int to, unsigned char *seen) {
    if (from == to) return 1;
    if (seen[from]) return 0;
    seen[from] = 1;
    for (int k = 0; k < MAX_DEPS; k++) {
        int d = t[from].dep_idx[k], gi = t[from].grp_dep_idx[k];
        if (d >= 0 && d < count && order_reaches(t, count, gr, d, to, seen)) return 1;
        if (gi >= 0 && gi < gr->n)
            for (int m = 0; m < gr->g[gi].member_count; m++) {
                int x = gr->g[gi].member_idx[m];
                if (x >= 0 && x < count && order_reaches(t, count, gr, x, to, seen)) return 1;
            }
    }
    for (int k = 0; k < t[from].order_count; k++)
        if (order_reaches(t, count, gr, t[from].order_idx[k], to, seen)) return 1;
    return 0;
}

static void order_add(service_t *t, int count, const order_grps_t *gr, int s, int target, const char *how) {
    unsigned char seen[MAX_SERVICES];
    if (target < 0 || target == s) return;
    for (int k = 0; k < t[s].order_count; k++)
        if (t[s].order_idx[k] == target) return;
    memset(seen, 0, sizeof seen);
    /* systemd drops an ordering job in a cycle; a waiting pair here would hold forever */
    if (order_reaches(t, count, gr, target, s, seen)) {
        fprintf(stderr, "[schema-init] %s: %s%s closes a cycle — ignored\n",
                t[s].name, how, t[target].name);
        return;
    }
    t[s].order_idx[t[s].order_count++] = target;
}

void services_resolve_order(service_t *table, int count, const group_t *groups, int gcount) {
    order_grps_t gr = { groups, groups ? gcount : 0 };
    int i, k;
    for (i = 0; i < count; i++) table[i].order_count = 0;
    for (i = 0; i < count; i++)
        for (k = 0; k < MAX_ORDER && table[i].after_name[k][0]; k++)
            order_add(table, count, &gr, i, svc_find(table, count, table[i].after_name[k]), "after=");
    for (i = 0; i < count; i++)
        for (k = 0; k < MAX_ORDER && table[i].before_name[k][0]; k++) {
            int j = svc_find(table, count, table[i].before_name[k]);
            char how[80];
            if (j < 0) continue;
            snprintf(how, sizeof how, "before=%s from ", table[i].before_name[k]);
            order_add(table, count, &gr, j, i, how);
        }
}

/* A queued service that can never start: waiting on a wait_socket= (outside
 * PID 1's hands), on a critical dep that is excised or dormant, an excised group,
 * or a dep that is itself stuck. systemd fails that start job, which lets go of
 * whatever is ordered after it. */
static int order_pred_stuck(const service_t *p, const service_t *t, int count,
                            const uint8_t *grp_states, int gcount, int depth) {
    if (p->sock_waiting) return 1;
    if (depth > MAX_SERVICES) return 0;
    for (int k = 0; k < MAX_DEPS && p->dep_name[k][0]; k++) {
        int d = p->dep_idx[k], gi = p->grp_dep_idx[k];
        if (d >= 0 && d < count) {
            uint8_t s = t[d].inst.state;
            if ((s == STATE_EXCISED || s == STATE_DORMANT) && (t[d].flags & SVC_CRITICAL)) return 1;
            if (s == STATE_NEW_PROCESS && !t[d].restart_count
                    && order_pred_stuck(&t[d], t, count, grp_states, gcount, depth + 1)) return 1;
        }
        if (gi >= 0 && gi < gcount && grp_states && grp_states[gi] == STATE_EXCISED) return 1;
    }
    return 0;
}

/* systemd's After= waits for the start job, not for health: a service still
 * queued or spawned-not-ready on its first start holds its followers; up,
 * given up on, failed once (in recovery, or retrying), parked (socket, timer,
 * condition skip) or unable to start does not. */
int service_order_ready(const service_t *svc, const service_t *table, int count,
                        const uint8_t *grp_states, int gcount) {
    for (int k = 0; k < svc->order_count; k++) {
        int j = svc->order_idx[k];
        if (j < 0 || j >= count) continue;
        uint8_t s = table[j].inst.state;
        /* restart_count > 0: a retry, its first start already finished */
        if (table[j].restart_count) continue;
        if (s == STATE_FULL_TRUST) return 0;
        if (s == STATE_NEW_PROCESS && !order_pred_stuck(&table[j], table, count, grp_states, gcount, 0))
            return 0;
    }
    return 1;
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
