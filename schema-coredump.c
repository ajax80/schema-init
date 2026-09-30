/* schema-coredump — kernel core_pattern pipe helper.
 *
 * The kernel runs this as root with the core on stdin and
 *   argv: %P %u %g %s %t %c %d
 * (global pid, uid, gid, signal, time, RLIMIT_CORE, dumpable). Each crash
 * gets a metadata record, core.<comm>.<uid>.<time>.<pid>.meta, and, unless
 * RLIMIT_CORE forbids it, a rate limit applies or space is short, a zstd core
 * beside it. Everything under COREDUMP_DIR is root-only: a core holds the
 * crashed process's memory, secrets included.
 *
 *   schema-coredump --take-pattern   replace systemd-coredump's pattern
 *   schema-coredump --list           one line per recorded crash */

#include "coredump.h"

#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <syslog.h>
#include <time.h>
#include <zstd.h>

#define CHUNK (128 * 1024)

struct crash {
    long pid, uid, gid, sig, time;
    unsigned long long climit;
    int dumpable;
    char comm[64], exe[PATH_MAX], cmdline[1024], cgroup[512], service[128], boot_id[40];
    char stem[256], core[300], reason[32];
    unsigned long long raw, stored;
    int truncated;
};

static void read_small(const char *path, char *buf, size_t sz, int nul_to_space) {
    buf[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sz - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    if (nul_to_space)
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] == '\0') buf[i] = ' ';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = '\0';
}

static void read_proc(struct crash *c) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%ld/comm", c->pid);
    read_small(p, c->comm, sizeof c->comm, 0);
    snprintf(p, sizeof p, "/proc/%ld/exe", c->pid);
    ssize_t n = readlink(p, c->exe, sizeof c->exe - 1);
    c->exe[n > 0 ? n : 0] = '\0';
    snprintf(p, sizeof p, "/proc/%ld/cmdline", c->pid);
    read_small(p, c->cmdline, sizeof c->cmdline, 1);
    snprintf(p, sizeof p, "/proc/%ld/cgroup", c->pid);
    read_small(p, c->cgroup, sizeof c->cgroup, 0);
    coredump_service_from_cgroup(c->cgroup, c->service, sizeof c->service);
    char *nl = strrchr(c->cgroup, '\n');
    if (nl) memmove(c->cgroup, nl + 1, strlen(nl + 1) + 1);
    read_small("/proc/sys/kernel/random/boot_id", c->boot_id, sizeof c->boot_id, 0);
    coredump_clean_value(c->comm);
    coredump_clean_value(c->exe);
    coredump_clean_value(c->cmdline);
    coredump_clean_value(c->cgroup);
    coredump_clean_value(c->service);
}

struct entry { char name[256]; time_t mtime; off_t size; };

static int entry_cmp(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    return (x->mtime > y->mtime) - (x->mtime < y->mtime);
}

static int ends_with(const char *s, const char *suf) {
    size_t l = strlen(s), sl = strlen(suf);
    return l >= sl && !strcmp(s + l - sl, suf);
}

/* Entries named core.*SUFFIX, oldest first. Caller frees. */
static struct entry *scan(int dfd, const char *suffix, size_t *count) {
    *count = 0;
    int fd = dup(dfd);
    if (fd < 0) return NULL;
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return NULL; }
    rewinddir(d);
    size_t cap = 0;
    struct entry *v = NULL;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "core.", 5) || !ends_with(de->d_name, suffix)) continue;
        if (strlen(de->d_name) >= sizeof v->name) continue;
        struct stat st;
        if (fstatat(dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) continue;
        if (*count == cap) {
            cap = cap ? cap * 2 : 64;
            struct entry *nv = realloc(v, cap * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        snprintf(v[*count].name, sizeof v->name, "%s", de->d_name);
        v[*count].mtime = st.st_mtime;
        v[*count].size = st.st_size;
        (*count)++;
    }
    closedir(d);
    if (v) qsort(v, *count, sizeof *v, entry_cmp);
    return v;
}

/* Another core from the same program inside the rate window: a crash loop
 * would otherwise write one core per restart. */
static int rate_limited(int dfd, const char *comm, time_t now) {
    size_t n;
    struct entry *v = scan(dfd, ".zst", &n);
    int hit = 0;
    for (size_t i = 0; i < n && !hit; i++) {
        char c[256];
        coredump_stem_comm(v[i].name, c, sizeof c);
        hit = !strcmp(c, comm) && now - v[i].mtime < COREDUMP_RATE_SECS;
    }
    free(v);
    return hit;
}

static unsigned long long room(int dfd) {
    struct statvfs sv;
    if (fstatvfs(dfd, &sv) != 0) return 0;
    unsigned long long total = (unsigned long long)sv.f_blocks * sv.f_frsize;
    unsigned long long avail = (unsigned long long)sv.f_bavail * sv.f_frsize;
    unsigned long long reserve = total / 100 * COREDUMP_FREE_PCT;
    return avail > reserve ? avail - reserve : 0;
}

/* Writers hold the directory lock, so a temp file seen under it belongs to a
 * helper that was killed mid-write. */
static void drop_stale_tmp(int dfd) {
    int fd = dup(dfd);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (!d) { if (fd >= 0) close(fd); return; }
    struct dirent *de;
    while ((de = readdir(d)))
        if (!strncmp(de->d_name, ".tmp.", 5)) unlinkat(dfd, de->d_name, 0);
    closedir(d);
}

static int store_core(int dfd, struct crash *c) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, ".tmp.%ld", c->pid);
    unlinkat(dfd, tmp, 0);
    int out = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) return -1;
    ZSTD_CCtx *z = ZSTD_createCCtx();
    static char in[CHUNK], zo[CHUNK];
    unsigned long long limit = c->climit < COREDUMP_RAW_MAX ? c->climit : COREDUMP_RAW_MAX;
    unsigned long long budget = room(dfd);
    if (budget > COREDUMP_DIR_MAX / 2) budget = COREDUMP_DIR_MAX / 2;
    int err = !z, eof = 0;
    if (z) ZSTD_CCtx_setParameter(z, ZSTD_c_compressionLevel, 3);
    while (!err && !eof) {
        size_t want = CHUNK;
        if (c->raw + want > limit) want = limit - c->raw;
        ssize_t n = want ? read(0, in, want) : 0;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { err = 1; break; }
        if (n == 0) {
            eof = 1;
            if (want == 0 || c->stored >= budget) c->truncated = 1;
        }
        c->raw += (unsigned long long)n;
        ZSTD_inBuffer ib = { in, (size_t)n, 0 };
        size_t left;
        do {
            ZSTD_outBuffer ob = { zo, sizeof zo, 0 };
            left = ZSTD_compressStream2(z, &ob, &ib, eof ? ZSTD_e_end : ZSTD_e_continue);
            if (ZSTD_isError(left) || write(out, zo, ob.pos) != (ssize_t)ob.pos) { err = 1; break; }
            c->stored += ob.pos;
        } while (eof ? left != 0 : ib.pos < ib.size);
        if (!eof && c->stored >= budget) {
            c->truncated = 1;
            limit = c->raw;
        }
    }
    ZSTD_freeCCtx(z);
    if (fsync(out) != 0) err = 1;
    close(out);
    snprintf(c->core, sizeof c->core, "%s.zst", c->stem);
    if (err || renameat(dfd, tmp, dfd, c->core) != 0) {
        unlinkat(dfd, tmp, 0);
        c->core[0] = '\0';
        return -1;
    }
    return 0;
}

/* Oldest cores go first until the total fits; then the oldest records. */
static void vacuum(int dfd, const char *keep) {
    size_t n;
    struct entry *v = scan(dfd, ".zst", &n);
    unsigned long long total = 0;
    for (size_t i = 0; i < n; i++) total += (unsigned long long)v[i].size;
    for (size_t i = 0; i < n && total > COREDUMP_DIR_MAX; i++) {
        if (!strcmp(v[i].name, keep)) continue;
        if (unlinkat(dfd, v[i].name, 0) == 0) total -= (unsigned long long)v[i].size;
    }
    free(v);
    v = scan(dfd, ".meta", &n);
    for (size_t i = 0; n > COREDUMP_KEEP && i < n - COREDUMP_KEEP; i++) {
        unlinkat(dfd, v[i].name, 0);
        char z[300];
        snprintf(z, sizeof z, "%.*s.zst", (int)(strlen(v[i].name) - 5), v[i].name);
        unlinkat(dfd, z, 0);
    }
    free(v);
}

static void write_meta(int dfd, const struct crash *c) {
    char tmp[64], name[300];
    snprintf(tmp, sizeof tmp, ".tmp.%ld.meta", c->pid);
    snprintf(name, sizeof name, "%s.meta", c->stem);
    unlinkat(dfd, tmp, 0);
    int fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    const char *sn = sigabbrev_np((int)c->sig);
    dprintf(fd,
            "PID=%ld\nUID=%ld\nGID=%ld\nSIGNAL=%ld\nSIGNAL_NAME=SIG%s\nTIME=%ld\nBOOT_ID=%s\n"
            "COMM=%s\nEXE=%s\nCMDLINE=%s\nCGROUP=%s\nSERVICE=%s\nDUMPABLE=%d\n"
            "REASON=%s\nCORE=%s\nCORE_RAW_BYTES=%llu\nCORE_STORED_BYTES=%llu\nTRUNCATED=%d\n",
            c->pid, c->uid, c->gid, c->sig, sn ? sn : "?", c->time, c->boot_id,
            c->comm, c->exe, c->cmdline, c->cgroup, c->service, c->dumpable,
            c->reason, c->core, c->raw, c->stored, c->truncated);
    if (fsync(fd) != 0 || close(fd) != 0 || renameat(dfd, tmp, dfd, name) != 0)
        unlinkat(dfd, tmp, 0);
}

static int open_dir(const char *dir, int create) {
    if (create && mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return -1;
    struct stat st;
    if (fstat(dfd, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 077)) {
        close(dfd);
        errno = EPERM;
        return -1;
    }
    return dfd;
}

static void meta_get(const char *text, const char *key, char *out, size_t sz) {
    size_t kl = strlen(key);
    out[0] = '\0';
    for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=') continue;
        size_t l = strcspn(p + kl + 1, "\n");
        if (l >= sz) l = sz - 1;
        memcpy(out, p + kl + 1, l);
        out[l] = '\0';
        return;
    }
}

static int list(const char *dir) {
    int dfd = open_dir(dir, 0);
    if (dfd < 0) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "schema-coredump: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    size_t n;
    struct entry *v = scan(dfd, ".meta", &n);
    printf("%-19s %8s %-8s %-24s %-12s %s\n", "TIME", "PID", "SIGNAL", "SERVICE/COMM", "RESULT", "CORE");
    for (size_t i = 0; i < n; i++) {
        char text[4096], t[24], pid[24], sig[16], svc[128], comm[64], reason[32], core[300], trunc[4];
        int fd = openat(dfd, v[i].name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t r = read(fd, text, sizeof text - 1);
        close(fd);
        text[r > 0 ? r : 0] = '\0';
        meta_get(text, "TIME", t, sizeof t);
        meta_get(text, "PID", pid, sizeof pid);
        meta_get(text, "SIGNAL_NAME", sig, sizeof sig);
        meta_get(text, "SERVICE", svc, sizeof svc);
        meta_get(text, "COMM", comm, sizeof comm);
        meta_get(text, "REASON", reason, sizeof reason);
        meta_get(text, "CORE", core, sizeof core);
        meta_get(text, "TRUNCATED", trunc, sizeof trunc);
        time_t tt = (time_t)atoll(t);
        char when[24] = "?";
        struct tm tm;
        if (localtime_r(&tt, &tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
        char who[200];
        snprintf(who, sizeof who, "%s%s%s", svc, svc[0] ? "/" : "", comm);
        printf("%-19s %8s %-8s %-24s %-12s %s%s%s\n", when, pid, sig, who,
               !strcmp(trunc, "1") ? "truncated" : reason, core[0] ? dir : "-",
               core[0] ? "/" : "", core);
    }
    free(v);
    close(dfd);
    return 0;
}

int main(int argc, char **argv) {
    for (int fd = 0; fd < 3; fd++)
        if (fcntl(fd, F_GETFD) < 0) {
            int nf = open("/dev/null", O_RDWR);
            if (nf >= 0 && nf != fd) { dup2(nf, fd); close(nf); }
        }
    umask(077);
    const char *dir = getenv("SCHEMA_COREDUMP_DIR");
    if (!dir || !*dir) dir = COREDUMP_DIR;

    if (argc == 2 && !strcmp(argv[1], "--take-pattern")) {
        int r = coredump_take_pattern();
        return r < 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--list")) return list(dir);
    if (argc != 8) {
        fprintf(stderr, "usage: schema-coredump --list | --take-pattern\n"
                        "       (as core_pattern) %%P %%u %%g %%s %%t %%c %%d\n");
        return 2;
    }

    struct crash c;
    memset(&c, 0, sizeof c);
    c.pid = atol(argv[1]);
    c.uid = atol(argv[2]);
    c.gid = atol(argv[3]);
    c.sig = atol(argv[4]);
    c.time = atol(argv[5]);
    c.climit = strtoull(argv[6], NULL, 10);
    c.dumpable = atoi(argv[7]);
    if (c.pid <= 0) return 2;
    read_proc(&c);

    char name[64];
    snprintf(name, sizeof name, "%s", c.comm);
    coredump_clean_name(name, sizeof name);
    snprintf(c.stem, sizeof c.stem, "core.%s.%ld.%ld.%ld", name, c.uid, c.time, c.pid);

    openlog("schema-coredump", LOG_PID, LOG_DAEMON);
    int dfd = open_dir(dir, 1);
    if (dfd < 0) {
        syslog(LOG_ERR, "%s[%ld] (%s) SIG%s: cannot open %s: %m", c.comm, c.pid, c.exe,
               sigabbrev_np((int)c.sig) ? sigabbrev_np((int)c.sig) : "?", dir);
        return 1;
    }
    flock(dfd, LOCK_EX);
    drop_stale_tmp(dfd);

    if (c.climit < 4096)
        snprintf(c.reason, sizeof c.reason, "rlimit");
    else if (rate_limited(dfd, name, (time_t)c.time))
        snprintf(c.reason, sizeof c.reason, "rate-limited");
    else if (room(dfd) < CHUNK)
        snprintf(c.reason, sizeof c.reason, "no-space");
    else if (store_core(dfd, &c) != 0)
        snprintf(c.reason, sizeof c.reason, "error");
    else
        snprintf(c.reason, sizeof c.reason, "stored");

    vacuum(dfd, c.core);
    write_meta(dfd, &c);
    const char *sn = sigabbrev_np((int)c.sig);
    if (c.core[0])
        syslog(LOG_ERR, "%s%s%s[%ld] (%s) killed by SIG%s, core %s/%s (%llu -> %llu bytes%s)",
               c.service, c.service[0] ? ": " : "", c.comm, c.pid, c.exe, sn ? sn : "?",
               dir, c.core, c.raw, c.stored, c.truncated ? ", truncated" : "");
    else
        syslog(LOG_ERR, "%s%s%s[%ld] (%s) killed by SIG%s, no core (%s)",
               c.service, c.service[0] ? ": " : "", c.comm, c.pid, c.exe, sn ? sn : "?", c.reason);
    close(dfd);
    return 0;
}
