#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/reboot.h>
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

#define SVC_DIR         "/etc/schema-init/services"
#define TICK_USEC       250000   /* 250ms main loop tick */
#define CTL_SOCK_PATH   "/run/schema-init.sock"

static service_t    services[MAX_SERVICES];
static int          svc_count = 0;
static group_t      groups[MAX_GROUPS];
static int          grp_count = 0;
static volatile int running   = 1;
static volatile int do_reboot = 0;
static schema_shm_t *shm_ptr = NULL;
static int          ctl_fd   = -1;
static int          sig_fd   = -1;
static int          system_under_pressure = 0;
static int          pressure_clear_ticks = 0;
static struct timespec init_start;

/* ── PID 1 essentials ───────────────────────────────────────────────── */

static void mount_pseudo(void) {
    /* only attempt mounts when we are PID 1 */
    if (getpid() != 1) return;

    /* kernel mounts rootfs ro for fsck; remount rw before anything else */
    mount(NULL, "/", NULL, MS_REMOUNT, NULL);

    mount("proc",    "/proc", "proc",     MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL);
    mount("sysfs",   "/sys",  "sysfs",    MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL);
    mount("devtmpfs","/dev",  "devtmpfs", MS_NOSUID|MS_STRICTATIME,     NULL);
    mount("tmpfs",   "/run",  "tmpfs",    MS_NOSUID|MS_NODEV,           "mode=0755");
    mount("cgroup2", "/sys/fs/cgroup", "cgroup2", MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_RELATIME, NULL);
    int cg_fd = open("/sys/fs/cgroup/cgroup.subtree_control", O_WRONLY);
    if (cg_fd >= 0) {
        write(cg_fd, "+cpu +memory", 12);
        close(cg_fd);
    }
    mkdir("/run/dbus",     0755);
    mkdir("/run/lock",     1777);
    mkdir("/run/shm",      1777);
    mkdir("/run/user",              0755);
    mkdir("/run/user/0",            0700);
    mkdir("/run/systemd",           0755);
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
    signal(SIGHUP,  SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
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

            if (WIFEXITED(status) && WEXITSTATUS(status) == 0
                && (services[i].flags & SVC_ONESHOT)) {
                /* clean one-shot exit → PERFECT */
                services[i].inst.state = STATE_PERFECT;
                clock_gettime(CLOCK_MONOTONIC, &services[i].stable_time);
                service_log(&services[i], "oneshot-done");
            } else if (services[i].flags & SVC_NO_RESTART) {
                services[i].inst.state = STATE_EXCISED;
                service_log(&services[i], "76-no-restart");
            } else {
                /* unexpected death → enter recovery arc */
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
}

static void execute_fuse_cmd(service_t *svc) {
    pid_t pid;
    if (!svc->fuse_cmd[0]) return;
    service_log(svc, "fuse-cmd-exec");
    pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", svc->fuse_cmd, NULL);
        _exit(127);
    }
}

/* ── schema tick for one service ────────────────────────────────────── */

static void tick_service(service_t *svc,
                         const uint8_t *grp_states, int gcount) {
    uint32_t flags;
    uint8_t  prev = svc->inst.state;
    time_t   now  = time(NULL);

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
            if (!(svc->flags & SVC_ONESHOT) && svc->child_pid > 0) {
                int ready = 0;
                if (svc->ready_path[0] && access(svc->ready_path, F_OK) == 0) {
                    ready = 1;
                } else if (now - svc->start_time >= svc->stable_secs) {
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
                if (!(svc->flags & SVC_CRITICAL) && svc->dormant_count > 4) {
                    service_log(svc, "76-excised");
                } else {
                    time_t delay = 300L << (svc->dormant_count - 1);
                    if (delay > 3600) delay = 3600;
                    svc->dormant_until = time(NULL) + delay;
                    svc->inst.state = STATE_DORMANT;
                    service_log(svc, "dormant");
                }
            }
            break;

        case STATE_DORMANT:
            if (time(NULL) >= svc->dormant_until) {
                svc->inst.state = STATE_NEW_PROCESS;
                service_log(svc, "dormant-wake");
            }
            break;

        case STATE_FUNDAMENTAL:
        case STATE_PERFECT:
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

    if (strcmp(line, "status") == 0) {
        ctl_writef(fd, "services: %d  groups: %d\n", svc_count, grp_count);
        for (i = 0; i < svc_count; i++)
            ctl_writef(fd, "  %-24s  pid=%-6d  state=%-14s  restarts=%d\n",
                services[i].name, (int)services[i].child_pid,
                state_name(services[i].inst.state), services[i].restart_count);

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

    } else if (strncmp(line, "add ", 4) == 0) {
        const char *path = line + 4;
        int j, k, ds;
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
                write(fd, ".\n", 2);
                return;
            }
        }
        k = 0;
        for (ds = 0; ds < MAX_DEPS; ds++) {
            if (!services[svc_count].dep_name[ds][0]) break;
            for (j = 0; j < svc_count; j++) {
                if (strcmp(services[j].name, services[svc_count].dep_name[ds]) == 0) {
                    services[svc_count].dep_idx[k++] = j;
                    break;
                }
            }
        }
        ctl_writef(fd, "ok: %s queued\n", services[svc_count].name);
        svc_count++;
        write(fd, ".\n", 2);
        return;

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
    ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ctl_fd < 0) return;
    set_nonblock(ctl_fd);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CTL_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(ctl_fd); ctl_fd = -1; return;
    }
    chmod(CTL_SOCK_PATH, 0600);
    listen(ctl_fd, 4);
}

static void ctl_poll(void) {
    char buf[256];
    int pos = 0, cfd;
    ssize_t n;
    struct timeval tv = {0, 100000}; /* 100ms receive timeout */
    if (ctl_fd < 0) return;
    cfd = accept(ctl_fd, NULL, NULL);
    if (cfd < 0) return;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (pos < (int)sizeof(buf) - 1) {
        n = recv(cfd, buf + pos, 1, 0);
        if (n <= 0) break;
        if (buf[pos++] == '\n') break;
    }
    if (pos > 0) {
        buf[pos] = '\0';
        ctl_cmd(cfd, buf);
    }
    close(cfd);
}

static void signalfd_init(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
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
    for (i = 0; i < svc_count; i++) {
        uint8_t s = services[i].inst.state;
        if (s != STATE_FUNDAMENTAL && s != STATE_PERFECT && s != STATE_EXCISED) {
            return TICK_USEC / 1000;
        }
    }
    return -1;
}

/* ── main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int i;
    const char *svc_dir = SVC_DIR;

    (void)argc;
    (void)argv;

    clock_gettime(CLOCK_MONOTONIC, &init_start);

    if (getpid() == 1) {
        mount_pseudo();
        cleanup_tmp_locks();
    }
    setup_signals();

    svc_count = services_load(svc_dir, services, MAX_SERVICES);
    if (svc_count == 0) {
        svc_count = services_load("./services", services, MAX_SERVICES);
    }

    grp_count = groups_load(svc_dir, groups, MAX_GROUPS);
    if (grp_count == 0) {
        grp_count = groups_load("./services", groups, MAX_GROUPS);
    }

    /* resolve group member names → service indices */
    {
        int g, m, s;
        for (g = 0; g < grp_count; g++) {
            for (m = 0; m < groups[g].member_count; m++) {
                for (s = 0; s < svc_count; s++) {
                    if (strcmp(services[s].name, groups[g].member_name[m]) == 0) {
                        groups[g].member_idx[m] = s;
                        break;
                    }
                }
            }
        }
    }

    /* resolve group deps in service dep_name arrays */
    {
        int s, d, g;
        for (s = 0; s < svc_count; s++) {
            for (d = 0; d < MAX_DEPS; d++) {
                if (!services[s].dep_name[d][0]) break;
                if (services[s].dep_idx[d] >= 0) continue; /* already a service dep */
                for (g = 0; g < grp_count; g++) {
                    if (strcmp(groups[g].name, services[s].dep_name[d]) == 0) {
                        services[s].grp_dep_idx[d] = g;
                        break;
                    }
                }
            }
        }
    }

    if (services_check_cycles(services, svc_count) > 0) {
        fprintf(stderr, "schema-init: dependency cycles detected — dropping to rescue shell\n");
        pid_t rsh = fork();
        if (rsh == 0) {
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
                        while (read(sig_fd, &ssi, sizeof(ssi)) == (ssize_t)sizeof(ssi))
                            ;
                        reap();
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
        for (i = 0; i < svc_count; i++)  svc_states[i] = services[i].inst.state;
        for (i = 0; i < svc_count; i++)
            tick_service(&services[i], grp_states, grp_count);
        groups_update(groups, grp_count, svc_states, svc_count);
        shm_update();
    }

    /* shutdown: hold briefly so viewers see system_state 13/14, then kill */
    usleep(500000);
    printf("[schema-init] shutting down\n");
    for (i = 0; i < svc_count; i++) {
        if (services[i].child_pid > 0)
            kill(services[i].child_pid, SIGTERM);
    }
    sleep(3);
    for (i = 0; i < svc_count; i++) {
        service_cgroup_kill(&services[i]);
    }

    if (ctl_fd >= 0) {
        close(ctl_fd);
        unlink(CTL_SOCK_PATH);
    }

    if (shm_ptr) {
        munmap(shm_ptr, sizeof(schema_shm_t));
        shm_unlink(SCHEMA_SHM_NAME);
    }

    if (getpid() == 1) {
        sync();
        if (do_reboot) {
            printf("[schema-init] PID 1 reboot\n");
            reboot(RB_AUTOBOOT);
        } else {
            printf("[schema-init] PID 1 poweroff\n");
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
