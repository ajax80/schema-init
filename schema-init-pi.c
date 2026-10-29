#include "pi_bootguard.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static char root[PATH_MAX / 2];
static int root_was_ro;

static const char *P(char *buf, const char *rel) {
    snprintf(buf, PATH_MAX, "%s%s", root, rel);
    return buf;
}

static void mkdirs(const char *rel) {
    char buf[PATH_MAX];
    P(buf, rel);
    for (char *p = buf + strlen(root) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(buf, 0755);
        *p = '/';
    }
    mkdir(buf, 0755);
}

static int write_all(int fd, const char *b, size_t n) {
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Streamed, not mmap'd: the Pi may be short on memory this early. */
static int files_differ(const char *a, const char *b) {
    struct stat sa, sb;
    int fa = open(a, O_RDONLY | O_CLOEXEC);
    if (fa < 0) return 0;
    int fb = open(b, O_RDONLY | O_CLOEXEC);
    if (fb < 0) { close(fa); return 1; }
    int diff = fstat(fa, &sa) != 0 || fstat(fb, &sb) != 0 || sa.st_size != sb.st_size;
    if (!diff) {
        char x[4096], y[4096];
        for (;;) {
            ssize_t nx = read(fa, x, sizeof x), ny = read(fb, y, sizeof y);
            if (nx != ny || (nx > 0 && memcmp(x, y, (size_t)nx) != 0)) { diff = 1; break; }
            if (nx <= 0) break;
        }
    }
    close(fa);
    close(fb);
    return diff;
}

/* 1 if a name in dir a is missing from b or differs; recurses into drop-in dirs (foo.svc.d/). */
static int dir_one_way(const char *a, const char *b, int depth) {
    DIR *d = opendir(a);
    if (!d) return 0;
    struct dirent *e;
    int diff = 0;
    while (!diff && (e = readdir(d))) {
        const char *n = e->d_name;
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) continue;
        /* promote() points these two at the known-good scripts, so they always differ by design */
        if (depth == 0 && (strcmp(n, "pi-bootok.svc") == 0 || strcmp(n, "pi-deadline.svc") == 0)) continue;
        char pa[PATH_MAX], pb[PATH_MAX];
        struct stat sa, sb;
        snprintf(pa, sizeof pa, "%s/%s", a, n);
        snprintf(pb, sizeof pb, "%s/%s", b, n);
        if (stat(pa, &sa) != 0) continue;
        if (stat(pb, &sb) != 0 || S_ISDIR(sa.st_mode) != S_ISDIR(sb.st_mode)) diff = 1;
        else if (S_ISDIR(sa.st_mode)) diff = depth < 1 && dir_one_way(pa, pb, depth + 1);
        else diff = files_differ(pa, pb);
    }
    closedir(d);
    return diff;
}

/* A half-written state must never replace a whole one: the rename only follows a durable write. */
static int write_state(const struct pi_state *st) {
    char dir[PATH_MAX], tmp[PATH_MAX], fin[PATH_MAX], buf[64];
    P(dir, "/var/lib/schema-init");
    P(tmp, "/var/lib/schema-init/pi-boot.tmp");
    P(fin, "/var/lib/schema-init/pi-boot");
    int n = pi_state_format(st, buf, sizeof buf);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int bad = n < 0 || write_all(fd, buf, (size_t)n) != 0 || fsync(fd) != 0;
    if (close(fd) != 0 || bad || rename(tmp, fin) != 0) {
        unlink(tmp);
        return -1;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return 0;
}

static void log_fallback(enum pi_target t, int attempts) {
    char path[PATH_MAX], line[96];
    int fd = open(P(path, "/var/log/schema-init/fallback.log"), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    int n = snprintf(line, sizeof line, "%lld fallback=%s attempts=%d\n", (long long)time(NULL),
                     t == PI_KNOWN_GOOD ? "known-good" : "systemd", attempts);
    write_all(fd, line, (size_t)n);
    fsync(fd);
    close(fd);
}

static void to_boot_log(void) {
    char path[PATH_MAX];
    int fd = open(P(path, "/var/log/schema-init/boot.log"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
}

static void kmsg(const char *m) {
    char path[PATH_MAX];
    int k = open(P(path, "/dev/kmsg"), O_WRONLY);
    if (k < 0) return;
    write_all(k, m, strlen(m));
    close(k);
}

/* A target that cannot exec must not stay in the state: bootok would promote it into known-good. */
static void exec_target(enum pi_target t, struct pi_state *st, int pid1, char **argv) {
    char path[PATH_MAX];
    if (t == PI_CURRENT) {
        to_boot_log();
        argv[0] = (char *)P(path, "/usr/bin/schema-init");
        execv(path, argv);
        log_fallback(PI_KNOWN_GOOD, st->attempts);
        st->last_target = PI_KNOWN_GOOD;
        if (write_state(st) == 0) {
            kmsg("<3>schema-init-pi: exec current failed, trying known-good\n");
            t = PI_KNOWN_GOOD;
        } else {
            kmsg("<3>schema-init-pi: exec current failed and cannot save boot state, booting systemd\n");
            t = PI_SYSTEMD;
        }
    }
    if (t == PI_KNOWN_GOOD) {
        char svc[PATH_MAX];
        to_boot_log();
        setenv("SCHEMA_SVC_DIR", P(svc, "/var/lib/schema-init/known-good/services"), 1);
        argv[0] = (char *)P(path, "/var/lib/schema-init/known-good/schema-init");
        execv(path, argv);
        unsetenv("SCHEMA_SVC_DIR");
        log_fallback(PI_SYSTEMD, st->attempts);
        st->attempts = 0;
        st->last_target = PI_SYSTEMD;
        write_state(st);
        kmsg("<3>schema-init-pi: exec known-good failed, booting systemd\n");
    }
    if (pid1) {
        int fd = open("/dev/console", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        /* systemd-fsck-root skips a rw root, so fsck.repair=yes needs / back the way the kernel mounted it */
        if (root_was_ro && mount("", "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) != 0)
            kmsg("<3>schema-init-pi: remount / ro failed, systemd gets a rw root\n");
    }
    P(path, "/lib/systemd/systemd");
    argv[0] = path;
    execv(path, argv);
}

int main(int argc, char **argv) {
    (void)argc;
    /* The kernel forwards unrecognised cmdline KEY=val words as env: only the known-good exec may set this. */
    unsetenv("SCHEMA_SVC_DIR");
    int pid1 = getpid() == 1;
    const char *r = pid1 ? NULL : getenv("SCHEMA_PI_ROOT");
    if (r) snprintf(root, sizeof root, "%s", r);

    struct statvfs sv0;
    root_was_ro = pid1 && statvfs("/", &sv0) == 0 && (sv0.f_flag & ST_RDONLY);
    if (pid1 && mount("", "/", NULL, MS_REMOUNT, NULL) != 0) {
        /* no counter means the deadline reboot loop never ends; systemd has no deadline */
        kmsg("<3>schema-init-pi: remount / rw failed, cannot save boot state, booting systemd\n");
        struct pi_state none = { 0, PI_SYSTEMD };
        exec_target(PI_SYSTEMD, &none, pid1, argv);
        for (;;) pause();
    }

    mkdirs("/var/lib/schema-init");
    mkdirs("/var/log/schema-init");

    char path[PATH_MAX], kg[PATH_MAX], buf[256];
    struct pi_state st;
    ssize_t n = 0;
    int fd = open(P(path, "/var/lib/schema-init/pi-boot"), O_RDONLY);
    if (fd >= 0) {
        n = read(fd, buf, sizeof buf - 1);
        close(fd);
    }
    buf[n > 0 ? n : 0] = 0;
    pi_state_parse(buf, &st);

    /* known-good is the whole set: a services-only edit that fails boot must still fall back to it */
    char ksv[PATH_MAX], sv[PATH_MAX];
    struct stat ks;
    int differs = 0;
    if (stat(P(kg, "/var/lib/schema-init/known-good/schema-init"), &ks) == 0) {
        P(ksv, "/var/lib/schema-init/known-good/services");
        P(sv, "/etc/schema-init/services");
        differs = files_differ(kg, P(path, "/usr/bin/schema-init")) ||
                  dir_one_way(ksv, sv, 0) || dir_one_way(sv, ksv, 0);
    }
    int before = st.attempts;
    enum pi_target t = pi_decide(&st, differs);
    if (write_state(&st) != 0) {
        kmsg("<3>schema-init-pi: cannot save boot state, booting systemd\n");
        t = PI_SYSTEMD;
    }
    if (t != PI_CURRENT) log_fallback(t, before);

    exec_target(t, &st, pid1, argv);
    for (;;) pause();
}
