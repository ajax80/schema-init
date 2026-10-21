#define FDCLOSE_EXTERN
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
#define MAX_MOUNTS      128      /* shutdown remount-ro sweep */
static uint64_t     last_stall_ms = 0;
static uint64_t     last_reclaim_ms = 0;
#define CPU_CHECK_MS        2000            /* critical cpu.pressure avg10 read rate */
static int          cpu_stalled = 0;
static int          stall_causes = 0;     /* 1 memory, 2 critical cpu; this episode */
static void reexec_check_done(int status);
static int reexec_start(int client, const char *path, char *err, size_t errsz);
#define REEXEC_CHECK_MS 5000
static int     reexec_check_out = -1;     /* read end of the dry run's stdout */
static int     reexec_client = -1;        /* the waiting schema-ctl connection */
static char    reexec_path[256];
static char    boot_argv0[256] = "/sbin/schema-init";
static int     reexec_target = -1;        /* the binary the dry run validated */

/* The signals that end PID 1. Blocked across execve: the kernel drops
 * SIG_DFL signals sent to PID 1, and execve resets our handlers to SIG_DFL. */
static void held_signals(sigset_t *set) {
    sigemptyset(set);
    sigaddset(set, SIGTERM);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGUSR1);
    sigaddset(set, SIGUSR2);
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
static int modprobe_one(const char *mod) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/sbin/modprobe", "modprobe", mod, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static unsigned pci_hex(const char *dir, const char *attr) {
    char path[300], buf[16] = "";
    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/%s", dir, attr);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof buf, f)) buf[0] = '\0';
    fclose(f);
    return (unsigned)strtoul(buf, NULL, 16);
}

static int sp5100_present(void) {
    static const unsigned ids[][2] = {
        { 0x1002, 0x4385 }, { 0x1022, 0x780b }, { 0x1022, 0x790b }, { 0x1d94, 0x790b },
    };
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        unsigned v = pci_hex(e->d_name, "vendor"), dv = pci_hex(e->d_name, "device");
        for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++)
            if (v == ids[i][0] && dv == ids[i][1]) { found = 1; break; }
    }
    closedir(d);
    return found;
}

/* sp5100_tco only drives the AMD/Hygon FCH SMBus devices in its PCI id
 * table; on anything else (an AMD guest on q35 included) modprobe costs a
 * fork+exec on the boot path and fails with "No such device". */
static void load_watchdog_module(void) {
    if (getpid() != 1) return;
    if (access("/sys/module/sp5100_tco", F_OK) == 0 || !sp5100_present()) return;
    if (modprobe_one("sp5100_tco") == 0)
        printf("[schema-init] loaded watchdog module (sp5100_tco)\n");
    else
        printf("[schema-init] watchdog module load failed (sp5100_tco)\n");
}

/* systemd-modules-load parity: modprobe every module named in the
 * /etc/modules-load.d/*.conf drop-ins (one module per line; blanks and
 * '#'/';' comments ignored). Lets a node declare e.g. `tun` so devtmpfs
 * creates /dev/net/tun at boot, instead of each service wrapper modprobing
 * by hand. Runs after mount_pseudo() has devtmpfs mounted. */
static void load_configured_modules(void) {
    if (getpid() != 1) return;
    glob_t g;
    if (glob("/etc/modules-load.d/*.conf", 0, NULL, &g) != 0) return;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        FILE *f = fopen(g.gl_pathv[i], "r");
        if (!f) continue;
        char line[256];
        int r;
        while ((r = svc_read_line(f, line, sizeof line)) != 0) {
            char *p = line;
            if (r < 0) {
                printf("[schema-init] %s: line longer than %zu bytes — ignored\n",
                       g.gl_pathv[i], sizeof line - 1);
                continue;
            }
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == ';' || *p == '\0') continue;
            p[strcspn(p, " \t\r\n")] = '\0';
            if (*p == '\0') continue;
            if (modprobe_one(p) == 0)
                printf("[schema-init] loaded module '%s'\n", p);
            else
                printf("[schema-init] module load failed: '%s'\n", p);
        }
        fclose(f);
    }
    globfree(&g);
}

static void rescue_boot(void) {
    char cl[4096] = "";
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { ssize_t n = read(fd, cl, sizeof(cl) - 1); if (n > 0) cl[n] = '\0'; close(fd); }
    if (!rescue_cmdline(cl)) return;
    fprintf(stderr, "[schema-init] rescue boot requested on the kernel command line\n");
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[schema-init] rescue shell: fork failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        service_reset_child_sigmask();
        setsid();
        int tty = open("/dev/console", O_RDWR);
        if (tty >= 0) {
            ioctl(tty, TIOCSCTTY, 1);
            dup2(tty, STDIN_FILENO);
            dup2(tty, STDOUT_FILENO);
            dup2(tty, STDERR_FILENO);
            if (tty > STDERR_FILENO) close(tty);
        }
        pid_t q = fork();
        if (q == 0) {
            execlp("plymouth", "plymouth", "--wait", "quit", (char *)NULL);
            _exit(1);
        }
        if (q > 0) waitpid(q, NULL, 0);
        printf("\n*** schema-init: rescue boot ***\n"
               "No services are running and /etc/fstab is not mounted (mount -a).\n"
               "Exit the shell to continue booting.\n\n");
        fflush(stdout);
        execl("/usr/sbin/sulogin", "sulogin", (char *)NULL);
        execl("/sbin/sulogin", "sulogin", (char *)NULL);
        execl("/bin/sh", "sh", (char *)NULL);
        _exit(1);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) ;
    fprintf(stderr, "[schema-init] rescue shell exited, continuing boot\n");
}

static void watchdog_init(void) {
    const char *paths[] = { "/dev/watchdog0", "/dev/watchdog", NULL };
    int i;
    for (i = 0; paths[i]; i++) {
        watchdog_fd = open(paths[i], O_WRONLY | O_CLOEXEC);
        if (watchdog_fd >= 0) {
            int timeout = 0;
            if (ioctl(watchdog_fd, WDIOC_GETTIMEOUT, &timeout) < 0) timeout = 0;
            watchdog_pet_ms = wd_pet_ms(timeout);
            printf("[schema-init] opened hardware watchdog (%s, timeout %ds, pet every %lldms)\n",
                   paths[i], timeout, (long long)watchdog_pet_ms);
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
    for (i = 0; i < svc_count && !notify_paused_since; i++) {
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
    mount("pstore",  "/sys/fs/pstore", "pstore",  MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL);
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

    /* make / rshared so mount events propagate — podman and GLib/KDE mount
     * monitors need this; systemd runs `mount --make-rshared /` at boot. Without
     * it / stays private and mount-watching userspace spins (plasmashell 100%). */
    mount(NULL, "/", NULL, MS_SHARED|MS_REC, NULL);
}

/* Records the kernel left in pstore (efi-pstore: dmesg of a panic/oops) belong
 * to the previous boot. Move them to disk so the next crash finds room in
 * NVRAM: a record is unlinked only after its copy is fsync'd. Runs after
 * modules load so ramoops/pstore-blk records are there too. Skipped when
 * fstab gives /var or /var/lib its own filesystem that is not mounted yet --
 * the copy would land under the mount point and vanish. The archive path is
 * left in /run so schema-doctor knows this boot harvested something. */
#define PSTORE_DIR     "/sys/fs/pstore"
#define PSTORE_ARCHIVE "/var/lib/schema-init/pstore"
#define PSTORE_MARKER  "/run/schema-init/pstore-harvested"

static int pstore_var_pending(void) {
    struct stat root, st;
    struct mntent *m;
    int pending = 0;
    FILE *f = setmntent("/etc/fstab", "r");
    if (!f || stat("/", &root) < 0) { if (f) endmntent(f); return 0; }
    while ((m = getmntent(f))) {
        if (strcmp(m->mnt_dir, "/var") && strcmp(m->mnt_dir, "/var/lib")) continue;
        if (stat(m->mnt_dir, &st) < 0 || st.st_dev == root.st_dev) pending = 1;
    }
    endmntent(f);
    return pending;
}

static int pstore_copy(int dirfd, const char *name, int outdir) {
    char buf[8192];
    ssize_t n;
    int ok = 0;
    int in = openat(dirfd, name, O_RDONLY | O_CLOEXEC);
    if (in < 0) return 0;
    int out = openat(outdir, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out >= 0) {
        ok = 1;
        while ((n = read(in, buf, sizeof buf)) != 0) {
            if (n < 0 || write(out, buf, n) != n) { ok = 0; break; }
        }
        if (fsync(out) < 0) ok = 0;
        close(out);
    }
    close(in);
    return ok;
}

static void pstore_harvest(void) {
    struct dirent *de;
    struct stat st;
    time_t oldest = 0;
    char dest[128];
    int moved = 0, kept = 0;

    DIR *d = opendir(PSTORE_DIR);
    if (!d) return;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (fstatat(dirfd(d), de->d_name, &st, 0) == 0 && S_ISREG(st.st_mode) &&
            (oldest == 0 || st.st_mtime < oldest))
            oldest = st.st_mtime;
    }
    if (oldest == 0 || pstore_var_pending() || stat("/var/lib/schema-init", &st) < 0) {
        closedir(d);
        return;
    }

    struct tm tm;
    localtime_r(&oldest, &tm);
    mkdir(PSTORE_ARCHIVE, 0700);
    strftime(dest, sizeof dest, PSTORE_ARCHIVE "/%Y%m%d-%H%M%S", &tm);
    mkdir(dest, 0700);
    int outdir = open(dest, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (outdir < 0) { closedir(d); return; }

    rewinddir(d);
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (pstore_copy(dirfd(d), de->d_name, outdir) &&
            unlinkat(dirfd(d), de->d_name, 0) == 0)
            moved++;
        else
            kept++;
    }
    fsync(outdir);
    close(outdir);
    closedir(d);
    mkdir(engine_paths.run_dir, 0755);
    FILE *mk = fopen(PSTORE_MARKER, "w");
    if (mk) { fprintf(mk, "%s\n", dest); fclose(mk); }
    fprintf(stderr, "schema-init: pstore: %d record(s) from the previous boot -> %s%s\n",
            moved, dest, kept ? " (some left in pstore)" : "");
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

static void sig_term(int s)   { (void)s; engine_running = 0; engine_do_reboot = 0; if (shm_ptr) shm_ptr->system_state = 13; }
static void sig_int(int s)    { (void)s; engine_running = 0; engine_do_reboot = 1; if (shm_ptr) shm_ptr->system_state = 14; }
static void sig_usr1(int s)   { (void)s; engine_running = 0; engine_do_reboot = 0; }  /* halt */
static void sig_usr2(int s)   { (void)s; engine_running = 0; engine_do_reboot = 0; }  /* poweroff */

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

static int critical_cpu_pressure(void) {
    int i;
    for (i = 0; i < svc_count; i++) {
        service_t *svc = &services[i];
        if (svc->priority == PRIO_CRITICAL && svc->child_pid > 0 &&
            read_cpu_pressure(svc->cgroup_path) > 5.0)
            return 1;
    }
    return 0;
}

/* The kernel NULs the last byte written, so the trigger must go out with its
 * terminator or the window loses a digit and the write fails EINVAL. */
static void psi_open(void) {
    psi_fd = open("/proc/pressure/memory", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (psi_fd >= 0 && write(psi_fd, PSI_MEM_TRIGGER, sizeof(PSI_MEM_TRIGGER)) < 0) {
        close(psi_fd);
        psi_fd = -1;
    }
    printf("[schema-init] memory pressure: %s\n",
           psi_fd >= 0 ? "psi trigger armed (" PSI_MEM_TRIGGER ")" : "avg10 polling");
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
    rec_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.src = REC_SRC_PID1;
    rec_copy(ev.kind, sizeof ev.kind, under_pressure ? "survival-enter" : "survival-leave");
    rec_send(REC_SOCK_PATH, &ev);
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

/* Reclaim cold pages from non-critical cgroups on any memory-stalled pass of a
 * survival episode, however it began. Peripheral services are frozen by then,
 * so their pages are cold and reclaim cleanly. */
static void reclaim_maybe(void) {
    uint64_t now = monotonic_ms();
    if (last_reclaim_ms && now - last_reclaim_ms < RECLAIM_MIN_GAP_MS) return;
    last_reclaim_ms = now;
    reclaim_pass();
}

/* runit's halt, reboot and poweroff (runit-init 0|6, which elogind runs too)
 * make /etc/runit/stopit executable, and /etc/runit/reboot as well for a
 * reboot, then send PID 1 SIGCONT. */
static void runit_stop_request(void) {
    struct stat st;
    if (stat("/etc/runit/stopit", &st) != 0 || !(st.st_mode & S_IXUSR))
        return;
    chmod("/etc/runit/stopit", 0);
    engine_do_reboot = stat("/etc/runit/reboot", &st) == 0 && (st.st_mode & S_IXUSR);
    printf("[schema-init] runit-init %s: %s\n", engine_do_reboot ? "6" : "0", engine_do_reboot ? "rebooting" : "powering off");
    if (shm_ptr) shm_ptr->system_state = engine_do_reboot ? 14 : 13;
    engine_running = 0;
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

/* ── re-exec: the new image ─────────────────────────────────────────── */

static reexec_svc_t reexec_svcs[MAX_SERVICES];
static int reexec_reply_fd = -1;
static char reexec_reply[160];

static int reexec_read_blob(int fd, reexec_global_t *g, int *nrs,
                            eviction_t *ev, int *nev, fdstore_ent *fs, int maxfs, int *nfs,
                            char *err, size_t errsz) {
    int d;
    FILE *f;
    if (lseek(fd, 0, SEEK_SET) < 0 || (d = dup(fd)) < 0) {
        snprintf(err, errsz, "state fd %d: %s", fd, strerror(errno));
        return -1;
    }
    if (!(f = fdopen(d, "r"))) {
        snprintf(err, errsz, "state fd %d: %s", fd, strerror(errno));
        close(d);
        return -1;
    }
    int rc = state_parse(f, g, reexec_svcs, MAX_SERVICES, nrs, ev, nev, fs, maxfs, nfs, err, errsz);
    fclose(f);
    return rc;
}

/* Everything the new image must prove before it touches anything: the config
 * parses without a cycle, the blob parses, and the two agree. */
static int reexec_validate(int blob_fd, reexec_global_t *g, int *nrs,
                           eviction_t *ev, int *nev, fdstore_ent *fs, int maxfs, int *nfs,
                           char *err, size_t errsz) {
    if (engine_load(engine_paths.svc_dir) > 0) {
        snprintf(err, errsz, "dependency cycle in %s", engine_paths.svc_dir);
        return -1;
    }
    if (svc_count <= 0) {
        snprintf(err, errsz, "no services loaded from %s", engine_paths.svc_dir);
        return -1;
    }
    if (reexec_read_blob(blob_fd, g, nrs, ev, nev, fs, maxfs, nfs, err, errsz) < 0)
        return -1;
    return reexec_compare(reexec_svcs, *nrs, services, svc_count, err, errsz);
}

/* --reexec-check: run by the old image as a plain child, never as PID 1. */
static int reexec_check_main(int blob_fd) {
    reexec_global_t g;
    eviction_t ev[MAX_EVICTIONS];
    int nrs, nev, nfs, rc;
    char err[320];
    /* stdout is the verdict pipe: the loader's chatter goes to stderr, so
     * the verdict is the only thing on it however much the loader says */
    int verdict = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    rc = reexec_validate(blob_fd, &g, &nrs, ev, &nev, NULL, 0, &nfs, err, sizeof err);
    fflush(stdout);
    if (rc < 0) {
        dprintf(verdict, "err: %s\n", err);
        return 1;
    }
    dprintf(verdict, "ok: %s\n", SCHEMA_INIT_VERSION);
    return 0;
}

/* The new image could not take the state. Hand it back to the image that
 * wrote it; that image runs this same path and adopts it. If that is gone
 * too, a clean reboot beats a PID 1 exit (panic, and a hang without a
 * hardware watchdog). */
static void reexec_fallback(int blob_fd, int oldexe_fd, int client_fd, const char *why) {
    char b[16], o[16];
    fprintf(stderr, "[schema-init] re-exec refused by the new image: %s; returning to the previous image\n", why);
    if (client_fd >= 0)
        ctl_writef(client_fd, "err: new image refused the state: %s; back on the previous image\n", why);
    if (oldexe_fd >= 0) {
        /* "-1": the previous image has nowhere further back to go, so it
         * adopts without the strict checks instead of refusing in a loop */
        char *av[] = { "/sbin/schema-init", "--reexec", b, o, NULL };
        snprintf(b, sizeof b, "%d", blob_fd);
        snprintf(o, sizeof o, "%d", -1);
        fcntl(oldexe_fd, F_SETFD, FD_CLOEXEC);
        fflush(NULL);
        fexecve(oldexe_fd, av, environ);
    }
    fprintf(stderr, "[schema-init] re-exec: previous image unavailable (%s); rebooting\n", strerror(errno));
    sync();
    reboot(RB_AUTOBOOT);
    _exit(1);
}

/* Close every descriptor the old image leaked across execve, keeping the
 * standard three and the ones the blob names. */
static void reexec_close_strays(const int *keep, int nkeep) {
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int strays[256], n = 0;
    if (!d) return;
    int dfd = dirfd(d);
    while ((e = readdir(d))) {
        char *end;
        long fd = strtol(e->d_name, &end, 10);
        if (*end || end == e->d_name || fd <= 2 || fd == dfd) continue;
        int k;
        for (k = 0; k < nkeep; k++)
            if (keep[k] == fd) break;
        if (k < nkeep) continue;
        strays[n++] = (int)fd;
        if (n == (int)(sizeof strays / sizeof strays[0])) {
            fd_close_detached(strays, n);
            n = 0;
        }
    }
    closedir(d);
    fd_close_detached(strays, n);
}

/* Sockets the old image held: CLOEXEC again, and only while they are still
 * what this .svc asks for. */
static void listen_adopt(service_t *svc) {
    int ok = svc->listen_open == svc->listen_count;
    for (int k = 0; ok && k < svc->listen_open; k++)
        ok = service_listen_matches(svc, k);
    if (!ok) {
        if (svc->listen_open)
            fprintf(stderr, "[schema-init] re-exec: %s: carried sockets do not match listen=, rebinding at next start\n", svc->name);
        int parked = svc->listen_open && svc->child_pid == 0 && svc->inst.state == STATE_PERFECT;
        for (int k = 0; k < svc->listen_open && k < MAX_LISTEN; k++)
            close(svc->listen_fd[k]);
        svc->listen_open = 0;
        if (svc->listen_count && service_listen_open(svc) < 0 && parked) {
            svc->inst.state = STATE_NEW_PROCESS;
            svc->listen_hold = 1;
        }
        return;
    }
    for (int k = 0; k < svc->listen_open; k++)
        fcntl(svc->listen_fd[k], F_SETFD, FD_CLOEXEC);
}

static void reexec_adopt(int blob_fd, int oldexe_fd) {
    reexec_global_t g;
    eviction_t ev[MAX_EVICTIONS];
    int nrs = 0, nev = 0, nfs = 0, adopted = 0, fresh = 0, rolled_back = oldexe_fd < 0;
    char err[320];
    fdstore_ent *fs = calloc(FDSTORE_MAX_TOTAL, sizeof *fs);
    if (!fs)
        fprintf(stderr, "[schema-init] re-exec: out of memory, fdstore dropped across re-exec\n");

    memset(&g, 0, sizeof g);
    g.fd_ctl = g.fd_notify = g.fd_watchdog = g.fd_client = g.fd_oldexe = -1;
    if (oldexe_fd >= 0) {
        if (reexec_validate(blob_fd, &g, &nrs, ev, &nev, fs, fs ? FDSTORE_MAX_TOTAL : 0, &nfs, err, sizeof err) < 0)
            reexec_fallback(blob_fd, oldexe_fd, g.fd_client, err);
    } else {
        /* We ARE the fallback: nowhere further back to go, so refusing would
         * loop. Adopt whatever the config and blob allow; only a blob we
         * cannot read at all stops us, since then there is nothing to adopt. */
        int cycles = engine_load(engine_paths.svc_dir);
        if (reexec_read_blob(blob_fd, &g, &nrs, ev, &nev, fs, fs ? FDSTORE_MAX_TOTAL : 0, &nfs, err, sizeof err) < 0)
            reexec_fallback(blob_fd, -1, g.fd_client, err);
        if (cycles > 0)
            fprintf(stderr, "[schema-init] re-exec fallback: dependency cycle in %s, adopting anyway\n", engine_paths.svc_dir);
        if (reexec_compare(reexec_svcs, nrs, services, svc_count, err, sizeof err) < 0)
            fprintf(stderr, "[schema-init] re-exec fallback: adopting despite: %s\n", err);
    }

    {
        static int keep[6 + MAX_SERVICES * MAX_LISTEN + FDSTORE_MAX_TOTAL];
        int nkeep = 0, nres;
        keep[nkeep++] = g.fd_ctl; keep[nkeep++] = g.fd_notify; keep[nkeep++] = g.fd_watchdog;
        keep[nkeep++] = g.fd_client; keep[nkeep++] = blob_fd; keep[nkeep++] = oldexe_fd;
        for (int j = 0; j < nrs; j++)
            for (int k = 0; k < reexec_svcs[j].rt.listen_open && k < MAX_LISTEN; k++)
                keep[nkeep++] = reexec_svcs[j].rt.listen_fd[k];
        nres = nkeep;
        for (int j = 0; j < nfs; j++)
            keep[nkeep++] = fs[j].fd;
        reexec_close_strays(keep, nkeep);
        /* a blob entry naming an fd PID 1 already holds would make the store
         * own, and later close, PID 1's own socket: skip it */
        for (int j = 0; j < nfs; j++) {
            int fd = fs[j].fd, dup = 0;
            for (int k = 0; k < nres && !dup; k++)
                dup = keep[k] == fd;
            for (int k = 0; k < fdstore_n && !dup; k++)
                dup = fdstore_v[k].fd == fd;
            if (dup) {
                fprintf(stderr, "[schema-init] re-exec: fdstore %s/%s: fd %d is not ours, skipped\n",
                        fs[j].svc, fs[j].name, fd);
                continue;
            }
            if (fcntl(fd, F_GETFD) < 0) {
                fprintf(stderr, "[schema-init] re-exec: fdstore %s/%s: fd %d is not open, skipped\n",
                        fs[j].svc, fs[j].name, fd);
                continue;
            }
            if (fdstore_add(fs[j].svc, fd, fs[j].name, FDSTORE_MAX_TOTAL) == 0)
                fcntl(fd, F_SETFD, FD_CLOEXEC);
            else
                fd_close_detached(&fd, 1);
        }
    }
    free(fs);

    ctl_fd = g.fd_ctl;
    notify_fd = g.fd_notify;
    fdclose_sock = notify_fd;
    /* the old image may have shut fd passing at the cap; this image starts
     * with fdclose_shut = 0, so fdclose_passrights(1) would skip it */
    if (notify_fd >= 0) {
        int on = 1;
        setsockopt(notify_fd, SOL_SOCKET, SO_PASSRIGHTS, &on, sizeof on);
    }
    watchdog_fd = g.fd_watchdog;
    if (ctl_fd >= 0) fcntl(ctl_fd, F_SETFD, FD_CLOEXEC);
    if (notify_fd >= 0) fcntl(notify_fd, F_SETFD, FD_CLOEXEC);
    if (watchdog_fd >= 0) {
        int timeout = 0;
        fcntl(watchdog_fd, F_SETFD, FD_CLOEXEC);
        if (ioctl(watchdog_fd, WDIOC_GETTIMEOUT, &timeout) < 0) timeout = 0;
        watchdog_pet_ms = wd_pet_ms(timeout);
        write(watchdog_fd, "\0", 1);
    }

    for (int i = 0; i < svc_count; i++) {
        int j;
        for (j = 0; j < nrs; j++)
            if (!strcmp(services[i].name, reexec_svcs[j].name)) break;
        if (j == nrs) { fresh++; continue; }
        svc_runtime_copy(&services[i], &reexec_svcs[j].rt);
        svc_carry_timer_done(&services[i], !reexec_svcs[j].timer);
        listen_adopt(&services[i]);
        adopted++;
    }
    /* a service gone from the config: its sockets go with it */
    for (int j = 0; j < nrs; j++) {
        int i;
        for (i = 0; i < svc_count; i++)
            if (!strcmp(services[i].name, reexec_svcs[j].name)) break;
        if (i < svc_count) continue;
        for (int k = 0; k < reexec_svcs[j].rt.listen_open && k < MAX_LISTEN; k++)
            close(reexec_svcs[j].rt.listen_fd[k]);
    }
    if (g.argv0[0] == '/')
        snprintf(boot_argv0, sizeof boot_argv0, "%s", g.argv0);
    memcpy(evictions, ev, sizeof(eviction_t) * (size_t)nev);
    eviction_count = nev;
    for (int k = 0; k < eviction_count; k++)
        evictions[k].deadline = mono_sec() + (evictions[k].deadline - time(NULL));
    init_start = g.init_start;
    if (g.nofile_soft)
        service_set_nofile_soft_at_boot((rlim_t)g.nofile_soft);
    system_under_pressure = g.under_pressure;
    last_stall_ms = g.last_stall_ms;
    last_reclaim_ms = g.last_reclaim_ms;

    printf("[schema-init] re-exec %s -> %s: %d adopted, %d new\n",
           g.version, SCHEMA_INIT_VERSION, adopted, fresh);
    if (g.fd_client >= 0) {
        fcntl(g.fd_client, F_SETFD, FD_CLOEXEC);
        reexec_reply_fd = g.fd_client;
        if (rolled_back)
            snprintf(reexec_reply, sizeof reexec_reply,
                     "err: rolled back to %s, %d services adopted\n",
                     SCHEMA_INIT_VERSION, adopted);
        else
            snprintf(reexec_reply, sizeof reexec_reply,
                     "ok: re-executed into %s, %d services adopted, %d new\n",
                     SCHEMA_INIT_VERSION, adopted, fresh);
    }
    close(blob_fd);
    if (oldexe_fd >= 0) close(oldexe_fd);
}

/* ── re-exec: the old image ─────────────────────────────────────────── */

/* A sealed memfd holding the live state, CLOEXEC: it is opened to the dry run
 * and the new image explicitly, never inherited by a service. */
static int reexec_write_blob(int client, int oldexe, char *err, size_t errsz) {
    reexec_global_t g;
    int fd = memfd_create("schema-init-state", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        snprintf(err, errsz, "memfd_create: %s", strerror(errno));
        return -1;
    }
    memset(&g, 0, sizeof g);
    snprintf(g.version, sizeof g.version, "%s", SCHEMA_INIT_VERSION);
    snprintf(g.argv0, sizeof g.argv0, "%s", boot_argv0);
    g.init_start = init_start;
    g.under_pressure = system_under_pressure;
    g.last_stall_ms = last_stall_ms;
    g.last_reclaim_ms = last_reclaim_ms;
    g.nofile_soft = (uint64_t)service_nofile_soft_at_boot();
    g.fd_ctl = ctl_fd;
    g.fd_notify = notify_fd;
    g.fd_watchdog = watchdog_fd;
    g.fd_client = client;
    g.fd_oldexe = oldexe;

    int d = dup(fd);
    FILE *f = d >= 0 ? fdopen(d, "w") : NULL;
    if (!f) {
        snprintf(err, errsz, "state blob: %s", strerror(errno));
        if (d >= 0) close(d);
        close(fd);
        return -1;
    }
    /* the blob keeps wall-clock deadlines, so older and newer images agree */
    eviction_t wall_ev[MAX_EVICTIONS];
    for (int k = 0; k < eviction_count; k++) {
        wall_ev[k] = evictions[k];
        wall_ev[k].deadline = time(NULL) + (evictions[k].deadline - mono_sec());
    }
    state_write(f, &g, services, svc_count, wall_ev, eviction_count, fdstore_v, fdstore_n);
    if (fclose(f) != 0 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0) {
        snprintf(err, errsz, "state blob: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void reexec_reply_end(const char *fmt, const char *arg) {
    if (reexec_client < 0) return;
    ctl_writef(reexec_client, fmt, arg);
    write(reexec_client, ".\n", 2);
    close(reexec_client);
    reexec_client = -1;
}

/* schema-ctl reexec [PATH]: refuse what can be refused now, then start the
 * dry run as a plain child and go back to the main loop. PID 1 never waits
 * on it: a blocked PID 1 starves the watchdog and stops reaping. */
static int reexec_start(int client, const char *path, char *err, size_t errsz) {
    struct stat st;
    int out[2], blob;
    if (!engine_running) {
        snprintf(err, errsz, "shutting down");
        return -1;
    }
    if (reexec_check_pid > 0) {
        snprintf(err, errsz, "a re-exec is already in progress");
        return -1;
    }
    if (system_under_pressure) {
        static const char *cause[] = { "cause unknown", "memory stall",
            "critical service starved for CPU", "memory stall + critical service starved for CPU" };
        snprintf(err, errsz, "under pressure (%s); retry after %d s without a stall",
                 cause[stall_causes & 3], THAW_DELAY_MS / 1000);
        return -1;
    }
    if (path[0] != '/' || strlen(path) >= sizeof reexec_path) {
        snprintf(err, errsz, "'%s' is not an absolute path", path);
        return -1;
    }
    /* One open file for the checks, the dry run and the commit: whatever
     * replaces the path in between, PID 1 becomes the binary that was
     * checked. O_PATH so a FIFO swapped in can't block PID 1 in open(). */
    if ((reexec_target = open(path, O_PATH | O_CLOEXEC)) < 0) {
        snprintf(err, errsz, "open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(reexec_target, &st) < 0 || !S_ISREG(st.st_mode) || !(st.st_mode & 0111)) {
        snprintf(err, errsz, "'%s' is not an executable file", path);
        close(reexec_target); reexec_target = -1;
        return -1;
    }
    if ((blob = reexec_write_blob(-1, -1, err, errsz)) < 0) {
        close(reexec_target); reexec_target = -1;
        return -1;
    }
    if (pipe2(out, O_CLOEXEC) < 0) {
        snprintf(err, errsz, "pipe: %s", strerror(errno));
        close(blob); close(reexec_target); reexec_target = -1;
        return -1;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errsz, "fork: %s", strerror(errno));
        close(blob); close(out[0]); close(out[1]); close(reexec_target); reexec_target = -1;
        return -1;
    }
    if (pid == 0) {
        char b[16];
        char *av[] = { (char *)path, "--reexec-check", b, NULL };
        service_reset_child_sigmask();
        service_restore_child_nofile();
        dup2(out[1], STDOUT_FILENO);
        fcntl(blob, F_SETFD, 0);
        fcntl(reexec_target, F_SETFD, 0);   /* a #! script needs it open */
        snprintf(b, sizeof b, "%d", blob);
        fexecve(reexec_target, av, environ);
        dprintf(STDOUT_FILENO, "err: exec %s: %s\n", path, strerror(errno));
        _exit(127);
    }
    close(out[1]);
    close(blob);
    set_nonblock(out[0]);
    reexec_check_out = out[0];
    reexec_check_pid = pid;
    reexec_check_killed = 0;
    reexec_check_due = (int64_t)monotonic_ms() + REEXEC_CHECK_MS;
    reexec_client = fcntl(client, F_DUPFD_CLOEXEC, 3);
    snprintf(reexec_path, sizeof reexec_path, "%s", path);
    printf("[schema-init] re-exec: dry run of %s (pid %d)\n", path, (int)pid);
    return 0;
}

/* Called from the main loop: a dry run past its deadline gets SIGKILL; reap()
 * then reports it. */
static void reexec_check_deadline(void) {
    if (reexec_check_pid > 0 && !reexec_check_killed &&
        (int64_t)monotonic_ms() >= reexec_check_due) {
        kill(reexec_check_pid, SIGKILL);
        reexec_check_killed = 1;
    }
}

/* Re-check what can have changed during the dry run, against a fresh blob:
 * a .svc edited, a new orphan, a cycle. The same checks the dry run made. */
static int reexec_recheck(int blob, char *err, size_t errsz) {
    reexec_global_t g;
    eviction_t ev[MAX_EVICTIONS];
    int nrs, nev, nfs, shadow_count, shadow_grp_count, rc;

    memset(shadow_services, 0, sizeof(service_t) * MAX_SERVICES);
    memset(shadow_groups, 0, sizeof(group_t) * MAX_GROUPS);
    shadow_count = services_load(engine_paths.svc_dir, shadow_services, MAX_SERVICES);
    if (shadow_count == 0)
        shadow_count = services_load("./services", shadow_services, MAX_SERVICES);
    shadow_grp_count = groups_load(engine_paths.svc_dir, shadow_groups, MAX_GROUPS);
    if (shadow_grp_count == 0)
        shadow_grp_count = groups_load("./services", shadow_groups, MAX_GROUPS);

    if (validate_and_resolve(shadow_services, shadow_count, shadow_groups, shadow_grp_count) > 0) {
        snprintf(err, errsz, "dependency cycle in %s", engine_paths.svc_dir);
        rc = -1;
    } else if ((rc = reexec_read_blob(blob, &g, &nrs, ev, &nev, NULL, 0, &nfs, err, errsz)) == 0) {
        rc = reexec_compare(reexec_svcs, nrs, shadow_services, shadow_count, err, errsz);
    }
    for (int k = 0; k < shadow_count; k++)
        for (int m = 1; m < MAX_ARGV; m++)
            if (shadow_services[k].argv[m]) { free(shadow_services[k].argv[m]); shadow_services[k].argv[m] = NULL; }
    for (int k = 0; k < shadow_count; k++) service_free_landlock(&shadow_services[k]);
    return rc;
}

static void set_cloexec(int fd, int on) {
    if (fd >= 0) fcntl(fd, F_SETFD, on ? FD_CLOEXEC : 0);
}

static void listen_set_cloexec(int on) {
    for (int i = 0; i < svc_count; i++)
        for (int k = 0; k < services[i].listen_open; k++)
            set_cloexec(services[i].listen_fd[k], on);
}

static void reexec_commit(void) {
    char err[320];
    sigset_t held, prev;
    int oldexe, blob;

    if (!engine_running) {
        reexec_reply_end("err: %s\n", "shutting down");
        return;
    }
    int q = fdclose_quiesce();
    if (q < 0) {
        reexec_reply_end("err: %s\n", q == -2
            ? "an fd closer outlived SIGKILL (stuck in D state); try again later, or reboot"
            : "an fd closer has not reached close() yet, try again");
        return;
    }
    notify_pause_track();
    oldexe = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);

    /* execve resets our handlers to SIG_DFL and the kernel drops SIG_DFL
     * signals sent to PID 1. Blocked, a reboot sent now stays pending and the
     * new image receives it once its handlers are in. */
    held_signals(&held);
    sigprocmask(SIG_BLOCK, &held, &prev);

    blob = reexec_write_blob(reexec_client, oldexe, err, sizeof err);
    if (blob >= 0 && reexec_recheck(blob, err, sizeof err) == 0) {
        char b[16], o[16];
        int fds[] = { ctl_fd, notify_fd, watchdog_fd, reexec_client, oldexe, blob };
        for (size_t k = 0; k < sizeof fds / sizeof fds[0]; k++)
            set_cloexec(fds[k], 0);
        listen_set_cloexec(0);
        fdstore_set_cloexec(0);
        fdclose_set_cloexec(0);
        snprintf(b, sizeof b, "%d", blob);
        snprintf(o, sizeof o, "%d", oldexe);
        printf("[schema-init] re-exec: %s\n", reexec_path);
        if (watchdog_fd >= 0)
            write(watchdog_fd, "\0", 1);
        fflush(NULL);
        {
            char *av[] = { reexec_path, "--reexec", b, o, NULL };
            fexecve(reexec_target, av, environ);
        }
        snprintf(err, sizeof err, "exec %s: %s", reexec_path, strerror(errno));
        for (size_t k = 0; k < sizeof fds / sizeof fds[0]; k++)
            set_cloexec(fds[k], 1);
        listen_set_cloexec(1);
        fdstore_set_cloexec(1);
        fdclose_set_cloexec(1);
    }
    sigprocmask(SIG_SETMASK, &prev, NULL);
    if (blob >= 0) close(blob);
    if (oldexe >= 0) close(oldexe);
    /* after the two above: either may hold a number a killed closer freed */
    fdclose_resume();
    close(reexec_target);
    reexec_target = -1;
    printf("[schema-init] re-exec refused: %s\n", err);
    reexec_reply_end("err: %s\n", err);
}

static void reexec_check_done(int status) {
    char out[512] = "", *line, *nl;
    ssize_t n, len = 0;

    while (len < (ssize_t)sizeof out - 1 &&
           (n = read(reexec_check_out, out + len, sizeof out - 1 - (size_t)len)) > 0)
        len += n;
    out[len] = '\0';
    close(reexec_check_out);
    reexec_check_out = -1;
    reexec_check_pid = 0;

    /* the verdict is the child's last line; the config loader talks first */
    while (len > 0 && out[len - 1] == '\n') out[--len] = '\0';
    line = (nl = strrchr(out, '\n')) ? nl + 1 : out;

    if (reexec_check_killed || WIFSIGNALED(status) || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || strncmp(line, "ok:", 3)) {
        close(reexec_target);
        reexec_target = -1;
    }
    if (reexec_check_killed) {
        reexec_reply_end("err: dry run of %s did not finish in 5 s\n", reexec_path);
    } else if (WIFSIGNALED(status)) {
        char why[96];
        snprintf(why, sizeof why, "dry run died on signal %d", WTERMSIG(status));
        reexec_reply_end("err: %s\n", why);
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || strncmp(line, "ok:", 3)) {
        reexec_reply_end("%s\n", line[0] ? line : "err: dry run failed without a reason");
    } else {
        reexec_commit();
    }
}

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
        engine_paths.svc_dir);
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

/* Signal every container process; sig 0 just counts the live ones. */
static int container_cgroups_signal(int sig) {
    glob_t g;
    size_t n;
    int i, have = 0, live = 0;

    memset(&g, 0, sizeof g);
    for (i = 0; CONTAINER_CGROUP_PROC_GLOBS[i]; i++) {
        if (glob(CONTAINER_CGROUP_PROC_GLOBS[i],
                 have ? GLOB_APPEND : 0, NULL, &g) == 0)
            have = 1;
    }
    if (!have) return 0;

    for (n = 0; n < g.gl_pathc; n++) {
        FILE *f = fopen(g.gl_pathv[n], "r");
        pid_t p;
        if (!f) continue;
        while (fscanf(f, "%d", &p) == 1)
            if (p > 1 && kill(p, sig) == 0) live++;
        fclose(f);
    }
    if (sig && live) shut_log("container SIGTERM sent");
    globfree(&g);
    return live;
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

/* Survival posture: enter on the first stalled pass, thaw after
 * THAW_DELAY_MS without one. With a psi trigger armed, memory stall is
 * the trigger alone; avg10's ~14 s decay would hold the thaw. */
static void pid1_loop_pressure(void) {
    int mem = loop_psi_fired || (psi_fd < 0 && read_system_mem_pressure() > 10.0);
    uint64_t now = monotonic_ms();
    /* avg10 only moves every 2 s; reading it faster returns the same value. */
    if ((int64_t)now >= cpu_check_due) {
        cpu_check_due = (int64_t)now + CPU_CHECK_MS;
        cpu_stalled = critical_cpu_pressure();
    }
    stall_causes |= (mem ? 1 : 0) | (cpu_stalled ? 2 : 0);
    int act = pressure_step(&system_under_pressure, now, &last_stall_ms,
                            mem || cpu_stalled);
    if (act == PRESSURE_ENTER)
        execute_survival_posture(1);
    else if (act == PRESSURE_EXIT) {
        execute_survival_posture(0);
        stall_causes = 0;
    }
    if (system_under_pressure && mem)
        reclaim_maybe();
}

static int pid1_on_reaped(pid_t pid, int status) {
    if (pid != reexec_check_pid)
        return 0;
    reexec_check_done(status);
    return 1;
}

static void pid1_loop_tail(void) {
    watchdog_pet();
    reexec_check_deadline();
}

static int pid1_ctl_extra(int fd, char *line) {
    if (strcmp(line, "reexec") == 0 || strncmp(line, "reexec ", 7) == 0) {
        char rerr[320];
        const char *path = line[6] ? line + 7 : boot_argv0;
        while (*path == ' ') path++;
        if (reexec_start(fd, path, rerr, sizeof rerr) == 0)
            return 2;                   /* the reply comes when the dry run ends */
        ctl_writef(fd, "err: %s\n", rerr);

    } else if (strcmp(line, "reboot") == 0) {
        ctl_writef(fd, "ok: rebooting\n");
        if (shm_ptr) shm_ptr->system_state = 14;
        engine_do_reboot = 1;
        engine_running = 0;

    } else if (strcmp(line, "poweroff") == 0) {
        ctl_writef(fd, "ok: powering off\n");
        if (shm_ptr) shm_ptr->system_state = 13;
        engine_do_reboot = 0;
        engine_running = 0;
    } else {
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    int i;
    const char *svc_dir = engine_paths.svc_dir;
    int have_dir = 0;
    int reexec_fd = -1, reexec_oldexe = -1, reexec_check_fd = -1;

    /* An initrd's systemd runs with umask 0 and switch-root hands that to us;
     * every service would inherit it and create world-writable files.
     * The kernel hands PID 1 no PATH either; sh falls back to a built-in
     * one, but Go and execvp-less code can't find helpers without it. */
    if (getpid() == 1) {
        umask(022);
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 0);
        /* No console (or a container) leaves 0-2 closed; the next socket or
         * file would land there and a child's stdio dup2 would clobber it. */
        int nfd;
        while ((nfd = open("/dev/null", O_RDWR)) >= 0 && nfd < 3)
            ;
        if (nfd > 2) close(nfd);
    }

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) {
            printf("schema-init %s\n", SCHEMA_INIT_VERSION);
            return 0;
        }
        /* Written by our own previous image, never by the kernel: it
         * appends only words it didn't consume, and none start "--reexec". */
        if (!strcmp(argv[i], "--reexec") && i + 2 < argc) {
            reexec_fd = atoi(argv[++i]);
            reexec_oldexe = atoi(argv[++i]);
            continue;
        }
        if (!strcmp(argv[i], "--reexec-check") && i + 1 < argc) {
            reexec_check_fd = atoi(argv[++i]);
            continue;
        }
        /* As PID 1 the kernel appends every boot-cmdline word it didn't
         * consume (rhgb, splash, quiet, plymouth.debug, ...) to our argv.
         * Those tokens are not ours: letting a stray one become svc_dir makes
         * services_load() open a bogus directory and silently start zero
         * services. Only a hand-run invocation may name a services dir; as
         * PID 1, ignore all positional arguments and keep the built-in engine_paths.svc_dir. */
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

    if (reexec_check_fd >= 0)
        return getpid() == 1 ? 1 : reexec_check_main(reexec_check_fd);

    if (argv[0] && argv[0][0] == '/' && strlen(argv[0]) < sizeof boot_argv0)
        snprintf(boot_argv0, sizeof boot_argv0, "%s", argv[0]);

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

    /* A re-exec skips what the first boot already did. cleanup_tmp_locks
     * above all: run again it deletes live X/Xwayland locks. */
    if (reexec_fd < 0) {
        /* Detach from controlling terminal to prevent receiving SIGINT on Ctrl+C */
        int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (fd >= 0) {
            ioctl(fd, TIOCNOTTY);
            close(fd);
        }
        mount_pseudo();
        cleanup_tmp_locks();
        load_watchdog_module();
        load_configured_modules();
        pstore_harvest();
        rescue_boot();
        watchdog_init();
    }
    /* Before the control socket opens or a single service spawns, so the
     * raised limit covers every descriptor PID 1 will ever hold. */
    service_raise_pid1_nofile();
    /* With the pattern ours, every service inherits a core limit the helper
     * can honour; a process that wants no core still lowers its own. Boot
     * only: a pattern set after boot is the admin's, and the raised core
     * limit survives execve anyway. */
    if (reexec_fd < 0 && coredump_take_pattern() == 1)
        coredump_raise_if_ours();
    engine_hooks.loop_pressure = pid1_loop_pressure;
    engine_hooks.loop_tail     = pid1_loop_tail;
    engine_hooks.after_sockets = psi_open;
    engine_hooks.ctl_extra     = pid1_ctl_extra;
    engine_hooks.shut_log      = shut_log;
    engine_hooks.on_sigcont    = runit_stop_request;
    engine_hooks.on_reaped     = pid1_on_reaped;
    setup_signals();

    if (reexec_fd >= 0)
        reexec_adopt(reexec_fd, reexec_oldexe);
    else if (engine_load(svc_dir) > 0) {
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

    engine_open(reexec_fd >= 0);
    if (reexec_fd < 0)
        schema_boot_log();

    engine_arm_timers(reexec_fd >= 0);

    if (reexec_fd >= 0) {
        /* Handlers are installed: let a reboot/poweroff sent during the swap,
         * held pending since the old image blocked it, arrive now. */
        sigset_t held;
        held_signals(&held);
        sigprocmask(SIG_UNBLOCK, &held, NULL);
        if (reexec_reply_fd >= 0) {
            ctl_writef(reexec_reply_fd, "%s", reexec_reply);
            write(reexec_reply_fd, ".\n", 2);
            close(reexec_reply_fd);
            reexec_reply_fd = -1;
        }
    }

    engine_run();

    /* shutdown: hold briefly so viewers see system_state 13/14, then kill */
    watchdog_close();
    usleep(500000);
    shut_console_open();
    shut_write("[schema-init] shutting down\n");
    /* Containers do not hang off a .svc, so SIGTERM them first, ahead of the
     * services -- a clean stop lets Frigate flush frigate.db before the
     * remount-ro below, instead of relying on the kill(-1) SIGKILL further on. */
    uint64_t ct = monotonic_ms();
    container_cgroups_signal(SIGTERM);
    /* Nothing is activated from here on, so a socket PID 1 still listens on
     * would take a client's connection and never answer it (KDE's printer
     * client hung on cups.sock this way). Drop PID 1's copies: a running
     * service keeps the fds it was handed and serves until it is stopped; a
     * parked one is refused at once. */
    engine_stop_services();
    while (container_cgroups_signal(0) && monotonic_ms() - ct < SHUT_GRACE_MS)
        usleep(20000);
    engine_kill_services();
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

    engine_release();
    shut_log("control socket and shm released");

    if (getpid() == 1) {
        fuse_abort_all();
        bounded_sync(10);
        shut_flush_log();        /* the sweep is the last thing that can lose it */
        remount_all_ro();
        if (engine_do_reboot) {
            shut_write("[schema-init] PID 1 reboot\n");
            reboot(RB_AUTOBOOT);
        } else {
            shut_write("[schema-init] PID 1 poweroff\n");
            reboot(RB_POWER_OFF);
        }
    }

    engine_free();

    return 0;
}
