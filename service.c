#include "service.h"
#include "notify.h"
#include "caps.h"
#include "ns.h"
#include <linux/capability.h>
#include "coredump.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
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
#include <sys/wait.h>
#include <sys/random.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stddef.h>
#include <glob.h>
#include <sys/statvfs.h>
#include "svc_dropins.h"

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
    struct timespec now;
    long mem;

    /* F9_RETRY_COUNT / F9_RETRY_WIN — monotonic: an NTP step can't stretch it */
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (svc->restart_count < svc->max_restarts)     f |= F9_RETRY_COUNT;
    if ((!svc->spawn_time_mono.tv_sec && !svc->spawn_time_mono.tv_nsec) ||
        now.tv_sec - svc->spawn_time_mono.tv_sec >= COOLDOWN_SECS)
        f |= F9_RETRY_WIN;

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

/* best-effort single-value cgroup write: silently ignores a missing file (the
 * controller isn't enabled on this cgroup) or a value the kernel rejects. */
static void cg_write(const char *cgroup_path, const char *file, const char *val) {
    char path[160];
    int fd;
    snprintf(path, sizeof(path), "%s/%s", cgroup_path, file);
    fd = open(path, O_WRONLY);
    if (fd < 0) return;
    write(fd, val, strlen(val));
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

    /* memory.high: soft cap 10% below the hard cap -> the kernel reclaims cold
     * pages under gentle back-pressure before ever hitting the OOM path. Pairs
     * with the PSI reclaim executive. Only when a hard mem_limit exists. */
    {
        long high = mem_high_bytes(svc->mem_limit_mb);
        if (high > 0) {
            n = snprintf(buf, sizeof(buf), "%ld\n", high);
            cg_write(svc->cgroup_path, "memory.high", buf);
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
        cgroup_tier_t tier = cgroup_tiering(svc->priority);
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
    } else
        return 0;
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
    if (path_under(svc->exec, dirs, n)) what = "exec";
    else if (path_under(svc->ready_path, dirs, n)) what = "ready_path";
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

static int landlock_check(service_t *svc) {
    int i;
    if (!svc->landlock_count) return 0;
    for (i = 0; i < svc->landlock_count; i++)
        if (landlock_beneath(svc->exec, svc->landlock[i])) break;
    if (i == svc->landlock_count) {
        fprintf(stderr, "[schema-init] %s: landlock does not cover exec — not loaded\n",
                svc->name);
        return -1;
    }
    if (!(svc->flags & SVC_NO_NEW_PRIVS) && svc->cap_restrict &&
        !(svc->cap_keep_mask & (1ULL << CAP_SYS_ADMIN))) {
        fprintf(stderr, "[schema-init] %s: landlock needs no_new_privs=1 when keep_caps "
                "drops CAP_SYS_ADMIN — not loaded\n", svc->name);
        return -1;
    }
    return 0;
}

static int hardening_finalize(service_t *svc) {
    static const char *const tmp[] = { "/tmp/", "/var/tmp/" };
    static const char *const home[] = { "/home/", "/root/", "/run/user/" };
    apply_hardening_defaults(svc);
    if (ns_hides(svc, HARD_PT, &svc->ns_private_tmp, "private_tmp", tmp, 2) != 0) return -1;
    if (ns_hides(svc, HARD_PH, &svc->ns_protect_home, "protect_home", home, 3) != 0) return -1;
    return landlock_check(svc);
}

int service_apply_hardening(const service_t *svc) {
    const char *step = "";
    if (apply_mount_ns(svc->ns_private_tmp, svc->ns_protect_system,
                       svc->ns_protect_home, &step) != 0) {
        dprintf(2, "[schema-init] HARDENING FAILED for %s: mount_ns: %s: %d\n",
                svc->name, step, errno);
        return -1;
    }
    if (svc->cap_restrict) {
        if (apply_capabilities(svc->cap_keep_mask) != 0) {
            dprintf(2, "[schema-init] HARDENING FAILED for %s: capabilities: %d\n",
                    svc->name, errno);
            return -1;
        }
    }
    if (svc->flags & SVC_NO_NEW_PRIVS) {
        if (apply_no_new_privs() != 0) {
            dprintf(2, "[schema-init] HARDENING FAILED for %s: no_new_privs: %d\n",
                    svc->name, errno);
            return -1;
        }
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
    FILE *f = fopen(path, "re");
    if (!f) return -1;
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
    for (int i = 0; argv[i] && n < max; i++) {
        const char *a = argv[i];
        if (a[0] == '$' && env_name_ok(a + 1, strlen(a + 1))) {
            const char *v = getenv(a + 1);
            n += split_words(v ? v : "", out + n, max - n, 1);
            continue;
        }
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

static void svc_run_pre(const service_t *svc, int privileged) {
    for (int i = 0; i < svc->exec_pre_count; i++) {
        const char *c = svc->exec_pre[i];
        size_t pl = strspn(c, "+-");
        int plus = memchr(c, '+', pl) != NULL, ignore = memchr(c, '-', pl) != NULL;
        if (plus != privileged) continue;
        char *w[33], *x[33], **argv = x;
        int nw = service_split_cmdline(c + pl, w, 32);
        if (nw <= 0) _exit(1);
        w[nw] = NULL;
        if (svc->expand_args) {
            x[service_expand_argv(w, x, 32)] = NULL;
        } else {
            memcpy(x, w, nw * sizeof *w);
            x[nw] = NULL;
        }
        pid_t p = fork();
        if (p == 0) {
            execv(argv[0], argv);
            _exit(127);
        }
        int st = 0;
        while (p > 0 && waitpid(p, &st, 0) < 0 && errno == EINTR) ;
        if (p < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            dprintf(2, "[schema-init] %s: exec_pre %s failed (%s %d)%s\n", svc->name, c,
                    p < 0 ? "fork" : WIFEXITED(st) ? "exit" : "signal",
                    p < 0 ? errno : WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st),
                    ignore ? ", ignored" : "");
            if (!ignore) _exit(1);
        }
    }
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
        if (dfd >= 0 && (fstat(dfd, &st) < 0 || (st.st_uid != 0 && st.st_uid != geteuid()) ||
                         ((st.st_mode & 022) && !(st.st_mode & S_ISVTX)))) {
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
static void svc_pass_listen(const service_t *svc) {
    int n = svc->listen_open, tmp[MAX_LISTEN];
    if (!n) {
        unsetenv("LISTEN_FDS");
        unsetenv("LISTEN_PID");
        unsetenv("LISTEN_FDNAMES");
        return;
    }
    for (int i = 0; i < n; i++)
        if ((tmp[i] = fcntl(svc->listen_fd[i], F_DUPFD, 3 + n)) < 0) _exit(126);
    for (int i = 0; i < n; i++) {
        if (dup2(tmp[i], 3 + i) < 0) _exit(126);
        close(tmp[i]);
    }
    char v[24], names[MAX_LISTEN * 72] = "";
    snprintf(v, sizeof v, "%d", n);
    setenv("LISTEN_FDS", v, 1);
    snprintf(v, sizeof v, "%d", (int)getpid());
    setenv("LISTEN_PID", v, 1);
    for (int i = 0; i < n; i++)
        snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s.socket",
                 i ? ":" : "", svc->name);
    setenv("LISTEN_FDNAMES", names, 1);
}

int service_spawn(service_t *svc) {
    int sync[2];
    pid_t pid;

    svc->ready_path_verified = 0;
    svc->ready_watched = 0;
    svc->ctl_killed = 0;
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
        char *file_env[64];
        int file_envc = 0;
        for (int i = 0; i < svc->env_file_count; i++) {
            const char *ef = svc->env_file[i];
            int opt = *ef == '-';
            int got = service_env_file_read(ef + opt, file_env + file_envc, 64 - file_envc);
            if (got < 0) {
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
        if (svc->priority == PRIO_CRITICAL) {
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
        if (svc->exec_pre_count) {
            svc_apply_env(svc, file_env, file_envc);
            svc_run_pre(svc, 1);
        }
        if (service_apply_hardening(svc) != 0)
            _exit(126);
        if (svc->run_uid) {
            if (!svc->ns_protect_home) {
                char xdg[48];
                snprintf(xdg, sizeof(xdg), "/run/user/%u", (unsigned)svc->run_uid);
                mkdir(xdg, 0700);
                chown(xdg, svc->run_uid, svc->run_gid);
                setenv("XDG_RUNTIME_DIR", xdg, 1);
            }
            if (initgroups(svc->run_user[0] ? svc->run_user : "nobody", svc->run_gid) != 0) {
                dprintf(2, "[schema-init] UID DROP FAILED for %s: initgroups: %d\n",
                        svc->name, errno);
                _exit(126);
            }
            if (setgid(svc->run_gid) != 0) {
                dprintf(2, "[schema-init] UID DROP FAILED for %s: setgid: %d\n",
                        svc->name, errno);
                _exit(126);
            }
        }
        if (svc->landlock_count) {
            const char *step = "";
            if (apply_landlock(svc->landlock, svc->landlock_rw,
                               svc->landlock_count, &step) != 0) {
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
        if (svc->exec_pre_count) {
            svc_apply_env(svc, file_env, file_envc);
            svc_run_pre(svc, 0);
        }
        if (svc->notify) setenv("NOTIFY_SOCKET", NOTIFY_SOCK_PATH, 1);
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
        svc_pass_listen(svc);
        close_range(3 + svc->listen_open, ~0U, 0);
        char **argv = svc->argv, *xargv[64];
        if (svc->expand_args) {
            xargv[service_expand_argv(svc->argv, xargv, 63)] = NULL;
            argv = xargv;
        }
        execv(svc->exec, argv);
        _exit(127);
    }

    close(sync[0]);
    svc->child_pid  = pid;
    svc->fork_state = 0;
    svc->last_start = time(NULL);
    svc->start_time = svc->last_start;
    clock_gettime(CLOCK_MONOTONIC, &svc->spawn_time_mono);
    if (!svc->boot_spawn.tv_sec && !svc->boot_spawn.tv_nsec)
        svc->boot_spawn = svc->spawn_time_mono;
    svc->last_pet   = svc->spawn_time_mono;
    svc->wd_abort_at = (struct timespec){0, 0};
    svc->wd_armed_sec = svc->notify && svc->watchdog_sec > 0 ? svc->watchdog_sec : 0;
    svc->notify_ready = 0;
    svc->notify_status[0] = '\0';
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

static int parse_partition(const char *val) {
    if (strcasecmp(val, "isolated") == 0) return PART_ISOLATED;
    if (strcasecmp(val, "root") == 0)     return PART_ROOT;
    return PART_MEMBER;
}

/* parse on_calendar into svc->timer_cal_{hour,min,dow,dom} and set the timer
 * flags. Accepts "HH:MM" (daily), "Mon HH:MM" (weekly), "15 HH:MM" (monthly).
 * Returns 0 on success, -1 on malformed input (caller leaves the service
 * non-timer so a typo can't silently schedule garbage). Field parsing is the
 * pure, unit-tested parse_calendar_fields() in service.h. */
static int parse_calendar(service_t *svc, const char *val) {
    int h, m, dw, dm;
    if (parse_calendar_fields(val, &h, &m, &dw, &dm) != 0) return -1;
    svc->timer_cal_hour = h;
    svc->timer_cal_min  = m;
    svc->timer_cal_dow  = dw;
    svc->timer_cal_dom  = dm;
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
    svc->allowed_slot_min = -1;
    svc->allowed_slot_max = -1;
    svc->max_restarts = MAX_RESTARTS;
    svc->timer_cal_hour = -1;
    svc->timer_cal_dow = -1;
    svc->timer_cal_dom = -1;
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

void service_free_landlock(service_t *svc) {
    for (int i = 0; i < svc->exec_pre_count; i++) {
        free(svc->exec_pre[i]);
        svc->exec_pre[i] = NULL;
    }
    svc->exec_pre_count = 0;
    for (int i = 0; i < svc->landlock_count; i++) {
        free(svc->landlock[i]);
        svc->landlock[i] = NULL;
    }
    svc->landlock_count = 0;
    svc->landlock_rw = 0;
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

struct parse_ctx {
    int argc;
    int dep_slot;
    int dropin;
};

static int dropin_flag(service_t *svc, const char *key, const char *val) {
    static const struct { const char *key; unsigned flag; } f[] = {
        { "oneshot", SVC_ONESHOT }, { "needs_root", SVC_NEEDS_ROOT },
        { "critical", SVC_CRITICAL }, { "no_restart", SVC_NO_RESTART },
        { "persistent", SVC_TIMER_PERSIST }, { "stop_first", SVC_STOP_FIRST },
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
        int keep = svc->argv[0] == svc->exec ? 1 : 0;
        for (int i = keep; i < MAX_ARGV; i++) {
            if (svc->argv[i] != svc->exec) free(svc->argv[i]);
            svc->argv[i] = NULL;
        }
        if (keep) svc->argv[0] = svc->exec;
        pc->argc = keep;
    } else if (strcmp(key, "env") == 0) {
        for (int i = 0; i < svc->env_count; i++) {
            free(svc->envp[i]);
            svc->envp[i] = NULL;
        }
        svc->env_count = 0;
    } else if (strcmp(key, "exec_pre") == 0) {
        for (int i = 0; i < svc->exec_pre_count; i++) {
            free(svc->exec_pre[i]);
            svc->exec_pre[i] = NULL;
        }
        svc->exec_pre_count = 0;
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
    } else if (strcmp(key, "landlock_ro") == 0 || strcmp(key, "landlock_rw") == 0) {
        landlock_clear(svc, key[10] == 'w');
    } else
        return 0;
    return 1;
}

/* One key=value line. 0 = ok, -1 = the service must be rejected. */
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
        if (!pc->dropin || pc->argc == 0) pc->argc = 1;
    } else if (strcmp(line, "args") == 0 && pc->argc < MAX_ARGV - 1) {
        while (*val == ' ' || *val == '\t') val++;
        svc->argv[pc->argc++] = strdup(val);
    } else if (strcmp(line, "env") == 0 && svc->env_count < MAX_ENV) {
        while (*val == ' ' || *val == '\t') val++;
        if (strchr(val, '='))
            svc->envp[svc->env_count++] = strdup(val);
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
        char *w[32];
        const char *cmd = val + strspn(val, " \t");
        cmd += strspn(cmd, "+-");
        int nw = service_split_cmdline(cmd, w, 32);
        int ok = nw > 0 && w[0][0] == '/' && svc->exec_pre_count < MAX_EXEC_PRE;
        for (int i = 0; i < nw; i++) free(w[i]);
        if (!ok) {
            fprintf(stderr, "[schema-init] %s: exec_pre=%s needs an absolute command with balanced quotes, at most %d per service\n",
                    svc->name[0] ? svc->name : path, val, MAX_EXEC_PRE);
            return -1;
        }
        svc->exec_pre[svc->exec_pre_count++] = strdup(val + strspn(val, " \t"));
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
    } else if (strcmp(line, "dep") == 0 && pc->dep_slot < MAX_DEPS) {
        strncpy(svc->dep_name[pc->dep_slot++], val, 63);
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
    else if (strcmp(line, "critical") == 0 && atoi(val))
        svc->flags |= SVC_CRITICAL;
    else if (strcmp(line, "no_restart") == 0 && atoi(val))
        svc->flags |= SVC_NO_RESTART;
    else if (strcmp(line, "stop_first") == 0 && atoi(val))
        svc->flags |= SVC_STOP_FIRST;
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
    else if (strcmp(line, "ready_path") == 0)
        strncpy(svc->ready_path, val, sizeof(svc->ready_path) - 1);
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
        svc->watchdog_sec = atoi(val);
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
        if (parse_calendar(svc, val) != 0)
            fprintf(stderr, "[schema-init] WARN: '%s' bad on_calendar='%s' "
                    "(want HH:MM, or 'Mon HH:MM' weekly, or 'DD HH:MM' "
                    "monthly) — ignoring\n", svc->name, val);
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
    return 0;
}

static int svc_parse_file(service_t *svc, struct parse_ctx *pc, const char *path) {
    char line[512];
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
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

/* Post-parse checks and defaults, then the name from the filename.
 * 0 = loadable, -1 = rejected (template file, no name, no exec, bad hardening). */
static int svc_finalize(service_t *svc, struct parse_ctx *pc, const char *path) {
    if (hardening_finalize(svc) != 0) return -1;
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
    svc->argv[pc->argc] = NULL;
    return 0;
}

static int svc_load_file(const char *path, service_t *svc) {
    struct parse_ctx pc = { 0, 0, 0 };
    char dropins[SVC_DROPIN_MAX][SVC_DROPIN_PATH];
    int nd = svc_dropin_list(path, dropins, SVC_DROPIN_MAX);
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

int services_load(const char *dir, service_t *table, int max) {
    DIR *d = opendir(dir);
    struct dirent *ent;
    int count = 0;
    int warned = 0;

    if (!d) return 0;

    while ((ent = readdir(d))) {
        char path[512];
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
        if (svc_load_file(path, &table[count]) == 0)
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
    return svc_load_file(path, svc);
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
