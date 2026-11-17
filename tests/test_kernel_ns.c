#include "../ns.h"
#include "../caps.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

/* CI runs make test unprivileged: enter a user namespace mapped to root */
static int become_root(void) {
    char m[64];
    int fd;
    uid_t u = getuid();
    gid_t g = getgid();
    if (geteuid() == 0) return 0;
    if (unshare(CLONE_NEWUSER) != 0) return -1;
    fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd >= 0) { (void)!write(fd, "deny", 4); close(fd); }
    snprintf(m, sizeof m, "0 %u 1", (unsigned)u);
    fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd < 0 || write(fd, m, strlen(m)) < 0) return -1;
    close(fd);
    snprintf(m, sizeof m, "0 %u 1", (unsigned)g);
    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd < 0 || write(fd, m, strlen(m)) < 0) return -1;
    close(fd);
    return 0;
}

/* the mount flag, not access(W_OK): in a user namespace the permission check
 * (real root's files, unmapped) answers EACCES before the read-only one */
static int erofs(const char *p) {
    struct statvfs sv;
    return statvfs(p, &sv) == 0 && (sv.f_flag & ST_RDONLY);
}

static int setup(uint8_t kernel, const char *const *rw, int n_rw) {
    const char *step = "";
    if (apply_mount_ns("kns", 0, 0, 0, kernel, rw, n_rw, &step) != 0) {
        fprintf(stderr, "apply_mount_ns: %s: %s\n", step, strerror(errno));
        return -1;
    }
    return 0;
}

static int case_tunables(void) {
    if (setup(NS_K_TUNABLES, NULL, 0)) return 10;
    if (!erofs("/proc/sys/kernel/hostname")) return 11;
    if (!erofs("/sys/kernel")) return 12;
    if (access("/proc/sysrq-trigger", F_OK) == 0 && !erofs("/proc/sysrq-trigger")) return 13;
    return 0;
}

static int case_tunables_rw(void) {
    const char *rw[] = { "/sys/kernel" };
    if (setup(NS_K_TUNABLES, rw, 1)) return 10;
    if (erofs("/sys/kernel")) return 11;
    if (!erofs("/sys/fs")) return 12;
    return 0;
}

static int case_cgroups(void) {
    if (setup(NS_K_CGROUPS, NULL, 0)) return 10;
    if (!erofs("/sys/fs/cgroup")) return 11;
    if (erofs("/proc/sys/kernel/hostname")) return 12;
    return 0;
}

static int case_modules(void) {
    DIR *d;
    struct dirent *e;
    if (setup(NS_K_MODULES, NULL, 0)) return 10;
    d = opendir("/usr/lib/modules");
    if (!d) return errno == ENOENT ? 0 : 11;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) return 12;
    closedir(d);
    return 0;
}

static int case_devices(int host_log) {
    struct stat st;
    struct statvfs sv;
    char l[64];
    int fd;
    ssize_t n;
    if (setup(NS_K_DEVICES, NULL, 0)) return 10;
    fd = open("/dev/null", O_WRONLY);
    if (fd < 0 || write(fd, "x", 1) != 1) return 11;
    close(fd);
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, l, 4) != 4) return 12;
    close(fd);
    if (stat("/dev/kmsg", &st) == 0 || errno != ENOENT) return 13;
    fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0 || strncmp(ptsname(fd), "/dev/pts/", 9)) return 14;
    close(fd);
    n = readlink("/dev/fd", l, sizeof l - 1);
    if (n < 0) return 15;
    l[n] = 0;
    if (strcmp(l, "/proc/self/fd")) return 15;
    if (statvfs("/dev", &sv) != 0 || !(sv.f_flag & ST_NOEXEC)) return 16;
    if (host_log && (lstat("/dev/log", &st) != 0 || !S_ISSOCK(st.st_mode))) return 17;
    if (stat("/dev/shm", &st) != 0 || !S_ISDIR(st.st_mode)) return 18;
    return 0;
}

static int case_hostname(void) {
    if (setup(NS_K_HOSTNAME, NULL, 0)) return 10;
    if (sethostname("schema-kns-test", 15) != 0) return 11;
    return 0;
}

static int case_caps(void) {
    if (drop_bounding(ns_kernel_cap_drop(NS_K_MODULES | NS_K_DEVICES)) != 0) return 10;
    if (prctl(PR_CAPBSET_READ, CAP_SYS_MODULE) != 0) return 11;
    if (prctl(PR_CAPBSET_READ, CAP_MKNOD) != 0) return 12;
    if (prctl(PR_CAPBSET_READ, CAP_SYS_RAWIO) != 0) return 13;
    if (prctl(PR_CAPBSET_READ, CAP_NET_ADMIN) != 1) return 14;
    return 0;
}

static int case_all(void) {
    const char *step = "";
    if (apply_mount_ns("kns", 0, PROTECT_SYSTEM_FULL, 0,
                       NS_K_TUNABLES | NS_K_MODULES | NS_K_CGROUPS | NS_K_DEVICES | NS_K_HOSTNAME,
                       NULL, 0, &step) != 0) {
        fprintf(stderr, "apply_mount_ns all: %s: %s\n", step, strerror(errno));
        return 10;
    }
    return 0;
}

static int run(const char *name, int (*fn)(void)) {
    int st;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        if (become_root() != 0) _exit(99);
        _exit(fn());
    }
    assert(waitpid(pid, &st, 0) == pid);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 99) return 99;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "test_kernel_ns: %s: child status %d\n", name,
                WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 1;
    }
    return 0;
}

static int host_log;
static int case_devices_w(void) { return case_devices(host_log); }

int main(void) {
    struct stat st;
    char before[256], after[256];
    int fail = 0, r;
    host_log = lstat("/dev/log", &st) == 0 && S_ISSOCK(st.st_mode);
    gethostname(before, sizeof before);

    r = run("tunables", case_tunables);
    if (r == 99) { printf("SKIP  test_kernel_ns (no userns)\n"); return 0; }
    fail |= r;
    fail |= run("tunables_rw", case_tunables_rw);
    fail |= run("cgroups", case_cgroups);
    fail |= run("modules", case_modules);
    fail |= run("devices", case_devices_w);
    fail |= run("hostname", case_hostname);
    fail |= run("caps", case_caps);
    fail |= run("all", case_all);

    gethostname(after, sizeof after);
    if (strcmp(before, after)) { fprintf(stderr, "test_kernel_ns: host hostname changed\n"); fail = 1; }
    if (fail) return 1;
    printf("all kernel-ns tests passed\n");
    return 0;
}
