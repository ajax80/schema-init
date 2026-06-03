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
    if (svc->restart_count < MAX_RESTARTS)          f |= F9_RETRY_COUNT;
    if (now - svc->last_start >= COOLDOWN_SECS)     f |= F9_RETRY_WIN;

    /* F9_FALL_EXISTS / F9_FALL_HEALTH — no fallback system yet, reserved */
    (void)table; (void)count;

    /* F9_MEM_FREE / F9_MEM_SUFF */
    mem = free_mem_kb();
    if (mem > 0)            f |= F9_MEM_FREE;
    if (mem >= MEM_MIN_KB)  f |= F9_MEM_SUFF;

    /* F9_ESC_PATH / F9_ESC_AUTH — no escalation path yet */

    /* F9_TIMEOUT_WIN / F9_TIMEOUT_EXT */
    if (svc->restart_count < MAX_RESTARTS) {
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
    if (svc->restart_count < MAX_RESTARTS) f |= F6_ERR_PATH;
    if (mem >= MEM_MIN_KB)                 f |= F6_ERR_RES;

    /* F6_ROLL_STATE / F6_ROLL_SAFE — no state snapshot yet, reserved */

    /* F6_ESC_LIMIT / F6_ESC_PATTERN */
    if (svc->restart_count < MAX_RESTARTS) f |= F6_ESC_LIMIT;
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
        write(sub_fd, "+cpu +memory", 12);
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

int service_spawn(service_t *svc) {
    int sync[2];
    pid_t pid;

    if (pipe(sync) < 0) return -1;

    pid = fork();
    if (pid < 0) { close(sync[0]); close(sync[1]); return -1; }

    if (pid == 0) {
        char c;
        setsid();
        close(sync[1]);
        read(sync[0], &c, 1);
        close(sync[0]);
        char log_path[256];
        snprintf(log_path, sizeof(log_path), "/run/log/schema-init/%s.log", svc->name);
        mkdir("/run/log", 0755);
        mkdir("/run/log/schema-init", 0755);
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
        if (fd < 0) {
            snprintf(log_path, sizeof(log_path), "./run/log/schema-init/%s.log", svc->name);
            mkdir("./run", 0755);
            mkdir("./run/log", 0755);
            mkdir("./run/log/schema-init", 0755);
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
        execv(svc->exec, svc->argv);
        _exit(127);
    }

    close(sync[0]);
    svc->child_pid  = pid;
    svc->last_start = time(NULL);
    svc->start_time = svc->last_start;
    clock_gettime(CLOCK_MONOTONIC, &svc->last_pet);
    svc->restart_count++;
    cgroup_assign(svc, pid);
    write(sync[1], "", 1);
    close(sync[1]);
    return 0;
}

void service_cgroup_kill(service_t *svc) {
    char path[160];
    int fd;

    if (!svc->cgroup_path[0]) return;

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

void service_log(const service_t *svc, const char *event) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    printf("[%02d:%02d:%02d] %-20s  %-12s  state=%-12s  wt=%d  pid=%d\n",
           t->tm_hour, t->tm_min, t->tm_sec,
           svc->name, event,
           state_name(svc->inst.state),
           svc->inst.weight,
           (int)svc->child_pid);
    fflush(stdout);
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
            }
        }
        fclose(f);

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
        }
    }
    fclose(f);

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
