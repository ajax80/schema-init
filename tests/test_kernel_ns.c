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
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

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
    if (access("/proc/mtrr", F_OK) == 0 && !erofs("/proc/mtrr")) return 14;
    if (access("/proc/timer_list", F_OK) == 0 && !erofs("/proc/timer_list")) return 15;
    {
        char b[16];
        int fd = open("/proc/kallsyms", O_RDONLY);
        if (fd >= 0 && read(fd, b, sizeof b) > 0) return 16;
        if (fd >= 0) close(fd);
    }
    {
        DIR *d = opendir("/sys/kernel/tracing");
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) return 17;
        if (d) closedir(d);
    }
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
    if (!erofs("/proc/sys/kernel/hostname") || !erofs("/proc/sys/kernel/domainname")) return 12;
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

static int lfd = -1;

static int case_network(void) {
    struct ifaddrs *ifa, *i;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    int s, c, up = 0;
    if (private_net() != 0) return 10;
    if (getifaddrs(&ifa) != 0) return 11;
    for (i = ifa; i; i = i->ifa_next) {
        if (strcmp(i->ifa_name, "lo")) return 12;
        if (i->ifa_flags & IFF_UP) up = 1;
    }
    freeifaddrs(ifa);
    if (!up) return 13;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 1) != 0) return 14;
    getsockname(s, (struct sockaddr *)&a, &al);
    c = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(c, (struct sockaddr *)&a, sizeof a) != 0) return 15;
    /* a listener PID 1 opened before the unshare (socket activation) still accepts */
    if (lfd >= 0) {
        int k = accept(lfd, NULL, NULL);
        if (k < 0) return 16;
        close(k);
    }
    return 0;
}

static int case_mdwe(void) {
    if (apply_mdwe() != 0) return errno == EINVAL ? 0 : 10;
    execl("/proc/self/exe", "kns", "rwx-probe", (char *)NULL);
    return 11;
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

int main(int argc, char **argv) {
    struct stat st;
    if (argc > 1 && !strcmp(argv[1], "rwx-probe")) {
        void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return m == MAP_FAILED ? 0 : 20;
    }
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
    fail |= run("mdwe", case_mdwe);
    {
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        socklen_t al = sizeof a;
        int c;
        lfd = socket(AF_INET, SOCK_STREAM, 0);
        assert(lfd >= 0 && bind(lfd, (struct sockaddr *)&a, sizeof a) == 0 && listen(lfd, 1) == 0);
        getsockname(lfd, (struct sockaddr *)&a, &al);
        /* the host side connects first: the backlog holds it for the child's accept */
        c = socket(AF_INET, SOCK_STREAM, 0);
        assert(connect(c, (struct sockaddr *)&a, sizeof a) == 0);
        fail |= run("network", case_network);
        close(c);
        close(lfd);
    }

    gethostname(after, sizeof after);
    if (strcmp(before, after)) { fprintf(stderr, "test_kernel_ns: host hostname changed\n"); fail = 1; }
    if (fail) return 1;
    printf("all kernel-ns tests passed\n");
    return 0;
}
