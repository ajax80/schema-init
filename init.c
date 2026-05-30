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
    mkdir("/run/dbus",     0755);
    mkdir("/run/lock",     1777);
    mkdir("/run/shm",      1777);
    mkdir("/run/user",              0755);
    mkdir("/run/user/0",            0700);
    mkdir("/run/systemd",           0755);
    mkdir("/run/systemd/shutdown",  0755);
    mkdir("/run/sshd",              0755);
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

/* ── schema tick for one service ────────────────────────────────────── */

static void tick_service(service_t *svc,
                         const uint8_t *grp_states, int gcount) {
    uint32_t flags;
    uint8_t  prev = svc->inst.state;
    time_t   now  = time(NULL);

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
            if (svc->child_pid > 0 && now - svc->start_time >= svc->stable_secs) {
                flags = service_probe_f8(svc, services, svc_count);
                schema_step(&svc->inst, flags);
                if (svc->inst.state != prev) {
                    clock_gettime(CLOCK_MONOTONIC, &svc->stable_time);
                    service_log(svc, "promote");
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
                service_log(svc, "76-excised");
                if (svc->flags & SVC_CRITICAL) {
                    fprintf(stderr, "schema-init: critical service %s excised — system friction\n",
                            svc->name);
                }
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

    } else if (strncmp(line, "start ", 6) == 0) {
        const char *name = line + 6;
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

    } else if (strncmp(line, "stop ", 5) == 0) {
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

/* ── main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int i;
    const char *svc_dir = SVC_DIR;

    (void)argc;
    (void)argv;

    clock_gettime(CLOCK_MONOTONIC, &init_start);

    if (getpid() == 1) mount_pseudo();
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
            ret = poll(fds, nfds, TICK_USEC / 1000);
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

    return 0;
}
