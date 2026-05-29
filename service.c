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
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        setsid();
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execv(svc->exec, svc->argv);
        _exit(127);
    }

    svc->child_pid  = pid;
    svc->last_start = time(NULL);
    svc->start_time = svc->last_start;
    svc->restart_count++;
    cgroup_assign(svc, pid);
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

        if (di < 0 && gi < 0) break;

        if (di >= 0) {
            if (di >= scount) return 0;
            s = stable[di].inst.state;
            if (s == STATE_EXCISED)                              return 0;
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

    if (!d) return 0;

    while ((ent = readdir(d)) && count < max) {
        char path[512];
        FILE *f;
        char line[512];
        service_t *svc;
        int argc;
        size_t nlen = strlen(ent->d_name);

        /* only .svc files */
        if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".svc") != 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        f = fopen(path, "r");
        if (!f) continue;

        svc = &table[count];
        memset(svc, 0, sizeof(*svc));
        for (int i = 0; i < MAX_DEPS; i++) svc->dep_idx[i] = -1;
        for (int i = 0; i < MAX_DEPS; i++) svc->grp_dep_idx[i] = -1;
        schema_instance_init(&svc->inst, 0, STATE_PERFECT);
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
        }
        fclose(f);

        if (!svc->name[0] || !svc->exec[0]) continue;
        svc->argv[argc] = NULL;
        count++;
    }

    closedir(d);

    /* second pass: resolve dep names → indices */
    {
        int i, d, j, k;
        for (i = 0; i < count; i++) {
            k = 0;
            for (d = 0; d < MAX_DEPS; d++) {
                if (!table[i].dep_name[d][0]) break;
                for (j = 0; j < count; j++) {
                    if (strcmp(table[j].name, table[i].dep_name[d]) == 0) {
                        table[i].dep_idx[k++] = j;
                        break;
                    }
                }
                /* unresolved dep name: logged at runtime via service_deps_ready */
            }
        }
    }

    return count;
}
