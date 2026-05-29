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

#define SVC_DIR         "/etc/schema-init/services"
#define TICK_USEC       250000   /* 250ms main loop tick */

static service_t    services[MAX_SERVICES];
static int          svc_count = 0;
static group_t      groups[MAX_GROUPS];
static int          grp_count = 0;
static volatile int running   = 1;
static volatile int do_reboot = 0;
static schema_shm_t *shm_ptr = NULL;

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

static void sig_child(int s)  { (void)s; }
static void sig_term(int s)   { (void)s; running = 0; do_reboot = 0; }
static void sig_int(int s)    { (void)s; running = 0; do_reboot = 1; }
static void sig_usr1(int s)   { (void)s; running = 0; do_reboot = 0; }  /* halt */
static void sig_usr2(int s)   { (void)s; running = 0; do_reboot = 0; }  /* poweroff */

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = sig_child;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

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
            /* promote to FUNDAMENTAL once stable for STABLE_SECS */
            if (svc->child_pid > 0 && now - svc->start_time >= STABLE_SECS) {
                flags = service_probe_f8(svc, services, svc_count);
                schema_step(&svc->inst, flags);
                if (svc->inst.state != prev)
                    service_log(svc, "promote");
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

    shm_init();
    schema_boot_log();

    while (running) {
        uint8_t grp_states[MAX_GROUPS];
        uint8_t svc_states[MAX_SERVICES];
        reap();
        for (i = 0; i < grp_count; i++)  grp_states[i] = groups[i].state;
        for (i = 0; i < svc_count; i++)  svc_states[i] = services[i].inst.state;
        for (i = 0; i < svc_count; i++)
            tick_service(&services[i], grp_states, grp_count);
        groups_update(groups, grp_count, svc_states, svc_count);
        shm_update();
        usleep(TICK_USEC);
    }

    /* shutdown: send SIGTERM to all children */
    printf("[schema-init] shutting down\n");
    for (i = 0; i < svc_count; i++) {
        if (services[i].child_pid > 0)
            kill(services[i].child_pid, SIGTERM);
    }
    sleep(3);
    for (i = 0; i < svc_count; i++) {
        if (services[i].child_pid > 0)
            kill(services[i].child_pid, SIGKILL);
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
