#include "verify_root.h"
#include "karg.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define INSPECT_MS 30000
#define MAXWORDS 256
#define OUT_CAP (16 << 20)

static void vadd(char (*v)[256], int *n, int max, const char *fmt, va_list ap) {
    if (*n < max) vsnprintf(v[(*n)++], 256, fmt, ap);
}

static void fail(root_report_t *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vadd(r->reasons, &r->nreasons, 16, fmt, ap);
    va_end(ap);
    r->fail = 1;
}

static void unres(root_report_t *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vadd(r->unres, &r->nunres, 8, fmt, ap);
    va_end(ap);
    r->unresolved = r->unres[0];
}

static int is_majmin(const char *s) {
    size_t a = strspn(s, "0123456789"), b = a && s[a] == ':' ? strspn(s + a + 1, "0123456789") : 0;
    return b && !s[a + 1 + b];
}

int dev_resolve(const char *spec, const char *devdir, char *out, size_t n) {
    static const struct { const char *key, *dir; } by[] = {
        { "UUID=", "by-uuid" }, { "PARTUUID=", "by-partuuid" },
        { "LABEL=", "by-label" }, { "PARTLABEL=", "by-partlabel" },
    };
    char path[PATH_MAX], real[PATH_MAX], name[1024];
#ifdef SCHEMA_VERIFY_TESTHOOKS
    if (!devdir) devdir = getenv("SCHEMA_VERIFY_DEVDIR");
#endif
    if (!devdir || !*devdir) devdir = "/dev";

    size_t i;
    for (i = 0; i < sizeof by / sizeof *by && strncmp(spec, by[i].key, strlen(by[i].key)); i++)
        ;
    if (i < sizeof by / sizeof *by) {
        /* UUID= with no value would name the by-uuid directory itself */
        if (!spec[strlen(by[i].key)]) return -1;
        /* udev's link names: anything outside this set is \xNN, so a value can't climb out with / */
        size_t o = 0;
        for (const unsigned char *p = (const unsigned char *)spec + strlen(by[i].key); *p; p++) {
            if (o + 5 > sizeof name) return -1;
            if (isalnum(*p) || strchr("#+-.:=@_", *p) || *p >= 0x80)
                name[o++] = i == 1 ? tolower(*p) : *p;
            else
                o += snprintf(name + o, 5, "\\x%02x", *p);
        }
        name[o] = '\0';
        if (snprintf(path, sizeof path, "%s/disk/%s/%s", devdir, by[i].dir, name) >= (int)sizeof path) return -1;
    } else if (!strncmp(spec, "/dev/", 5) && spec[5]) {
        if (snprintf(path, sizeof path, "%s/%s", devdir, spec + 5) >= (int)sizeof path) return -1;
    } else if (is_majmin(spec)) {
        if (snprintf(path, sizeof path, "%s/block/%s", devdir, spec) >= (int)sizeof path) return -1;
    } else {
        return -1;
    }
    if (!realpath(path, real)) return -1;
    return snprintf(out, n, "%s", real) >= (int)n ? -1 : 0;
}

int bls_path_map(const char *mnt, const char *p, char *out, size_t n) {
    if (p[0] != '/') return -1;
    for (const char *s = p; (s = strstr(s, "/..")); s += 3)
        if (!s[3] || s[3] == '/') return -1;
    size_t ml = strlen(mnt);
    while (ml && mnt[ml - 1] == '/') ml--;
    return snprintf(out, n, "%.*s%s", (int)ml, mnt, p) >= (int)n ? -1 : 0;
}

int stat_in_root(const char *root, const char *path, int nosym, struct stat *st) {
    char chk[PATH_MAX];
    if (bls_path_map("/", path, chk, sizeof chk)) return -2;
    const char *rel = path + strspn(path, "/");
    int rfd = open(root, O_PATH | O_DIRECTORY | O_CLOEXEC), rc = -2;
    if (rfd < 0) return -2;
    struct open_how how = { .flags = O_PATH | O_CLOEXEC,
                            .resolve = RESOLVE_IN_ROOT | (nosym ? RESOLVE_NO_SYMLINKS : 0) };
    int fd = syscall(SYS_openat2, rfd, *rel ? rel : ".", &how, sizeof how);
    if (fd >= 0) {
        rc = fstat(fd, st) ? -2 : 0;
        close(fd);
    } else if (errno == ENOENT || errno == ENOTDIR) {
        rc = -1;
    } else if (errno == ENOSYS) {
        /* no openat2: walk it without following anything, and call any symlink unknown */
        char pre[PATH_MAX];
        rc = 0;
        for (size_t i = 0; rc == 0 && (i == 0 || rel[i - 1]); i++) {
            if (rel[i] && rel[i] != '/') continue;
            snprintf(pre, sizeof pre, "%.*s", (int)i, rel);
            if (fstatat(rfd, *pre ? pre : ".", st, AT_SYMLINK_NOFOLLOW))
                rc = errno == ENOENT || errno == ENOTDIR ? -1 : -2;
            else if (S_ISLNK(st->st_mode))
                rc = -2;
            if (!rel[i]) break;
        }
    }
    close(rfd);
    return rc;
}

int boot_is_separate(const char *mountinfo) {
    FILE *f = fopen(mountinfo, "r");
    if (!f) return 0;
    char line[4096], mp[4096];
    int sep = 0;
    while (!sep && fgets(line, sizeof line, f))
        sep = sscanf(line, "%*s %*s %*s %*s %4095s", mp) == 1 && !strcmp(mp, "/boot");
    fclose(f);
    return sep;
}

static const char *env_lookup(grubenv_t *env, int n, const char *k) {
    for (int i = n - 1; i >= 0; i--)
        if (!strcmp(env[i][0], k)) return env[i][1];
    return NULL;
}

/* -1: unknown variable (named in missing); -2: too long once expanded; -3: its grubenv value was cut */
int opts_expand(const char *s, grubenv_t *env, int nenv, char *out, size_t n, char *missing, size_t msz) {
    size_t o = 0;
    while (*s) {
        if (*s != '$') {
            if (o + 1 >= n) return -2;
            out[o++] = *s++;
            continue;
        }
        const char *nm = s + 1, *end;
        int br = *nm == '{';
        if (br) {
            nm++;
            if (!(end = strchr(nm, '}'))) {
                snprintf(missing, msz, "%s", s + 1);
                return -1;
            }
        } else {
            for (end = nm; isalnum((unsigned char)*end) || *end == '_'; end++)
                ;
        }
        snprintf(missing, msz, "%.*s", (int)(end - nm), nm);
        const char *v = end > nm ? env_lookup(env, nenv, missing) : NULL;
        if (!v) return -1;
        if (!strcmp(v, GRUBENV_CUT)) return -3;
        size_t vl = strlen(v);
        if (o + vl >= n) return -2;
        memcpy(out + o, v, vl);
        o += vl;
        s = end + br;
    }
    out[o] = '\0';
    return 0;
}

/* first occurrence before "--"; karg_check has already refused duplicates */
static const char *argval(char (*w)[512], int n, const char *key) {
    size_t kl = strlen(key);
    for (int i = 0; i < n && strcmp(w[i], "--"); i++)
        if (!strncmp(w[i], key, kl)) return w[i] + kl;
    return NULL;
}

int entry_precheck(const bls_entry_t *e, grubenv_t *env, int nenv, char *opts, size_t optsz, root_report_t *r) {
    memset(r, 0, sizeof *r);
    r->check_rc = -1;
    if (e->blank_before_last) fail(r, "blank line before the entry's last key: GRUB drops what follows it");
    if (e->parse_err) fail(r, "entry could not be read whole (a line or the initrd list is too long)");
    if (!e->linux_path[0]) fail(r, "no linux line");
    if (e->extra_linux) unres(r, "several linux lines");
    if (e->extra_options) unres(r, "several options lines");

    char miss[256];
    int x = opts_expand(e->options, env, nenv, opts, optsz, miss, sizeof miss);
    if (x == -1) {
        unres(r, "options use $%s, not in grubenv", miss);
        return -1;
    }
    if (x == -2) {
        unres(r, "options too long once grubenv variables are expanded");
        return -1;
    }
    if (x == -3) {
        unres(r, "grubenv value of $%s is too long to read whole", miss);
        return -1;
    }
    char (*w)[512] = malloc(MAXWORDS * sizeof *w), why[256], dev[PATH_MAX];
    int nw = karg_split(opts, w, MAXWORDS), rc = -1;
    if (nw < 0) {
        unres(r, "more than %d kernel arguments", MAXWORDS);
    } else if (karg_check(w, nw, why, sizeof why)) {
        fail(r, "%s", why);
    } else if (dev_resolve(argval(w, nw, "root="), NULL, dev, sizeof dev)) {
        fail(r, "root=%s does not resolve to a present device", argval(w, nw, "root="));
    } else {
        /* dracut reads SCHEMA_SVC_DIR= itself and switch_root may not hand it to PID 1;
         * init= it does pass on */
        if (e->n_initrd && argval(w, nw, "SCHEMA_SVC_DIR="))
            unres(r, "SCHEMA_SVC_DIR with an initramfs: not known to reach PID 1");
        rc = 0;
    }
    if (nw >= 0 && argval(w, nw, "root=") && !e->n_initrd) {
        /* no initramfs: the kernel resolves root= itself, with no udev and no device-mapper
         * (blakbox 09-16: root=UUID= with no initrd panicked) */
        const char *rs = argval(w, nw, "root=");
        if (e->initrd_vars)
            unres(r, "initrd is only GRUB variables: whether an initramfs loads is not known");
        else if (strncmp(rs, "PARTUUID=", 9) && strncmp(rs, "PARTLABEL=", 10) && !is_majmin(rs) &&
                 (strncmp(rs, "/dev/", 5) || strchr(rs + 5, '/')))
            fail(r, "root=%s with no initrd: the kernel alone resolves only PARTUUID=, PARTLABEL=, /dev/<name> "
                    "or major:minor, so this boot panics", rs);
    }
    free(w);
    return rc;
}

static int run(char *const argv[], int ofd, int efd) {
    static char *const envp[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
    pid_t p = fork();
    if (p == 0) {
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, STDIN_FILENO);
        dup2(ofd, STDOUT_FILENO);
        dup2(efd, STDERR_FILENO);
        execve(argv[0], argv, envp);
        _exit(127);
    }
    int st;
    if (p < 0 || waitpid(p, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static void first_line(int fd, char *buf, size_t n) {
    ssize_t m = pread(fd, buf, n - 1, 0);
    buf[m > 0 ? m : 0] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
}

static int is_btrfs(const char *dev) {
    char magic[8] = "";
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t m = pread(fd, magic, sizeof magic, 0x10040);
    close(fd);
    return m == 8 && !memcmp(magic, "_BHRfS_M", 8);
}

/* not btrfs: try each block filesystem the kernel knows, as mount(8) does */
static int mount_any(const char *dev, const char *dir, unsigned long fl, char *type, size_t tsz) {
    FILE *f = fopen("/proc/filesystems", "r");
    char line[128];
    int ok = -1;
    while (ok && f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "nodev", 5)) continue;
        snprintf(type, tsz, "%s", line + strspn(line, " \t"));
        type[strcspn(type, "\n")] = '\0';
        if (strcmp(type, "btrfs")) ok = mount(dev, dir, type, fl, NULL);
    }
    if (f) fclose(f);
    return ok;
}

#define F(...) dprintf(ctl, "F " __VA_ARGS__)
#define U(...) dprintf(ctl, "U " __VA_ARGS__)

static int check_file(int ctl, const char *grub, const char *what, const char *p) {
    struct stat st;
    int rc = stat_in_root(grub, p, 0, &st);
    if (rc == -2) return U("%s path %s could not be resolved\n", what, p);
    if (rc || !S_ISREG(st.st_mode)) return F("%s %s is missing\n", what, p);
    return 0;
}

int boot_files_check(int ctl, const char *grub, const bls_entry_t *e) {
    int bad = !!check_file(ctl, grub, "kernel", e->linux_path);
    for (int i = 0; i < e->n_initrd; i++) bad |= !!check_file(ctl, grub, "initrd", e->initrd[i]);
    return bad;
}

int subvol_check(int ctl, const char *top, const char *sv, const char *rootspec) {
    char p[PATH_MAX];
    struct stat st;
    sv += strspn(sv, "/");
    int svl = (int)strcspn(sv, ",");
    snprintf(p, sizeof p, "/%.*s", svl, sv);
    int rc = stat_in_root(top, p, 1, &st);
    if (rc == -2) return U("subvolume path %.*s has a symlink or can't be read\n", svl, sv);
    /* 256 is the inode of every btrfs subvolume's root directory */
    if (rc || !S_ISDIR(st.st_mode)) return F("subvolume %.*s does not exist on %s\n", svl, sv, rootspec);
    if (st.st_ino != 256) return F("%.*s on %s is a directory, not a subvolume\n", svl, sv, rootspec);
    return 0;
}

int init_resolve(int ctl, const char *tgt, const char *init, char *real, size_t n) {
    char chk[PATH_MAX], fdp[64], host[PATH_MAX];
    if (bls_path_map("/", init, chk, sizeof chk)) return U("init=%s is not an absolute path inside the target\n", init);
    int rfd = open(tgt, O_PATH | O_DIRECTORY | O_CLOEXEC);
    struct open_how how = { .flags = O_PATH | O_CLOEXEC, .resolve = RESOLVE_IN_ROOT };
    int fd = rfd < 0 ? -1 : syscall(SYS_openat2, rfd, init + strspn(init, "/"), &how, sizeof how), e = errno;
    if (rfd >= 0) close(rfd);
    if (fd < 0 && (e == ENOENT || e == ENOTDIR)) return F("init=%s does not exist in the target\n", init);
    if (fd < 0) return U("init=%s could not be resolved in the target: %s\n", init, strerror(e));
    struct stat st;
    snprintf(fdp, sizeof fdp, "/proc/self/fd/%d", fd);
    ssize_t hl = readlink(fdp, host, sizeof host - 1);
    int bad = fstat(fd, &st);
    close(fd);
    size_t tl = strlen(tgt);
    if (hl < 0 || bad || (size_t)hl < tl || strncmp(host, tgt, tl)) return U("init=%s could not be resolved in the target\n", init);
    host[hl] = '\0';
    snprintf(real, n, "%s", host[tl] ? host + tl : "/");
    /* one record per line */
    if (strchr(real, '\n')) return U("init=%s resolves to a path with a newline\n", init);
    /* those are replaced by the host's or a tmpfs before init runs */
    static const char *const repl[] = { "/dev", "/proc", "/tmp" };
    for (size_t i = 0; i < sizeof repl / sizeof *repl; i++) {
        size_t rl = strlen(repl[i]);
        if (!strncmp(real, repl[i], rl) && (!real[rl] || real[rl] == '/'))
            return U("init=%s resolves into %s, which the inspection replaces\n", init, repl[i]);
    }
    if (!S_ISREG(st.st_mode) || !(st.st_mode & 0111)) return F("init=%s is not executable in the target\n", init);
    return 0;
}

static const char *const target_sub[] = { "/dev", "/proc", "/tmp" };

int target_dirs_check(int ctl, const char *tgt) {
    struct stat st;
    for (size_t i = 0; i < sizeof target_sub / sizeof *target_sub; i++)
        if (stat_in_root(tgt, target_sub[i], 1, &st) || !S_ISDIR(st.st_mode))
            return U("%s in the target is not a directory: nothing is mounted on it\n", target_sub[i]);
    return 0;
}

static void unescape(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
            *o++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 3;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

int mountinfo_find(const char *mountinfo, dev_t dev, char *mp, size_t n) {
    FILE *f = fopen(mountinfo, "r");
    if (!f) return -1;
    char line[8192], root[4096], m[4096];
    unsigned ma, mi;
    int rc = -1;
    while (rc && fgets(line, sizeof line, f))
        if (sscanf(line, "%*s %*s %u:%u %4095s %4095s", &ma, &mi, root, m) == 4 && makedev(ma, mi) == dev &&
            !strcmp(root, "/")) {
            unescape(m);
            rc = snprintf(mp, n, "%s", m) >= (int)n ? -1 : 0;
        }
    fclose(f);
    return rc;
}

/* copies the --check report in cfd to out, after a B record for schema-bootok's exec= */
static void bootok_exec(int ctl, int cfd, int out) {
    struct stat st;
    char *m;
    if (fstat(cfd, &st) || !st.st_size) return;
    if ((m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, cfd, 0)) == MAP_FAILED) return;
    static const char key[] = "\nschema-bootok.exec=";
    char *v = memmem(m, st.st_size, key, sizeof key - 1), path[PATH_MAX];
    if (v) {
        v += sizeof key - 1;
        size_t n = 0;
        while (v + n < m + st.st_size && v[n] != '\n' && v[n] != '\t') n++;
        snprintf(path, sizeof path, "%.*s", (int)n, v);
        struct stat xs;
        dprintf(ctl, "B %d\n", !stat(path, &xs) && S_ISREG(xs.st_mode) && (xs.st_mode & 0111) ? 1 : 2);
    }
    for (off_t o = 0; o < st.st_size;) {
        ssize_t w = write(out, m + o, st.st_size - o);
        if (w <= 0) break;
        o += w;
    }
    munmap(m, st.st_size);
}

static void inspect(const bls_entry_t *e, const char *opts, const char *extra, const char *base, int ctl, int out) {
    char (*w)[512] = malloc(MAXWORDS * sizeof *w);
    int nw = karg_split(opts, w, MAXWORDS);
    const char *rootspec = argval(w, nw, "root="), *flags = argval(w, nw, "rootflags=");
    const char *init = argval(w, nw, "init=");
    if (!init) init = "/sbin/init";
    char dev[PATH_MAX] = "", top[64], tgt[64], type[64] = "btrfs";
    unsigned long fl = MS_RDONLY | MS_NOSUID | MS_NODEV;
    if (dev_resolve(rootspec, NULL, dev, sizeof dev)) {
        U("root=%s stopped resolving during the inspection\n", rootspec);
        return;
    }

    /* the mounts must never show outside this child */
    if (unshare(CLONE_NEWNS) || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)) {
        U("cannot make a private mount namespace: %m\n");
        return;
    }
    /* tmpfs on the mkdtemp dir so the mountpoints below write nothing to the host */
    if (mount("schema-verify", base, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700")) {
        U("cannot mount a private tmpfs: %m\n");
        return;
    }
    snprintf(top, sizeof top, "%s/top", base);
    snprintf(tgt, sizeof tgt, "%s/root", base);
    mkdir(top, 0700);
    mkdir(tgt, 0700);

    int btrfs = is_btrfs(dev), bound = 0;
    struct stat ds;
    char mp[PATH_MAX];
    if (!btrfs && !stat(dev, &ds) && !mountinfo_find("/proc/self/mountinfo", ds.st_rdev, mp, sizeof mp)) {
        /* already mounted read-write: a second, read-only mount of it is EBUSY; a read-only
         * bind of that mount is the same tree (rootflags= only matters to btrfs here) */
        if (mount(mp, top, NULL, MS_BIND, NULL) ||
            mount(NULL, top, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL)) {
            U("cannot bind root=%s (%s, mounted on %s) read-only: %m\n", rootspec, dev, mp);
            return;
        }
        bound = 1;
    } else if (btrfs ? mount(dev, top, "btrfs", fl, "subvolid=5") : mount_any(dev, top, fl, type, sizeof type)) {
        U("cannot mount root=%s (%s) read-only: %m\n", rootspec, dev);
        return;
    }
    const char *sv = NULL;
    for (const char *p = flags; p && *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] == ','))
        if (!strncmp(p, "subvol=", 7)) sv = p + 7;
    if (btrfs && sv && subvol_check(ctl, top, sv, rootspec)) return;
    /* the target as the kernel mounts it: rootflags= is the mount data */
    /* a failed bind says nothing about the entry; a failed mount with rootflags= does */
    if (bound && (mount(top, tgt, NULL, MS_BIND, NULL) ||
                  mount(NULL, tgt, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL))) {
        U("cannot bind root=%s read-only onto the target: %m\n", rootspec);
        return;
    }
    if (!bound && mount(dev, tgt, type, fl, flags)) {
        F("cannot mount root=%s with rootflags=%s: %m\n", rootspec, flags ? flags : "");
        return;
    }

    /* separate /boot: that partition is GRUB's root; else the root device's top level */
    boot_files_check(ctl, boot_is_separate("/proc/self/mountinfo") ? "/boot" : top, e);

    /* before host /dev and /proc are bound in: init=/proc/self/exe must not resolve to verify itself */
    char real[PATH_MAX];
    if (init_resolve(ctl, tgt, init, real, sizeof real) || target_dirs_check(ctl, tgt)) return;
    dprintf(ctl, "I %s\n", real);

    /* always chroot, even into the running root's subvolume: the fresh mount carries
     * none of the host's submounts (/home, a split /usr), which the kernel won't have
     * either; user=/group= and absolute symlinks resolve against the target */
    for (size_t i = 0; i < sizeof target_sub / sizeof *target_sub; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s%s", tgt, target_sub[i]);
        if (i < 2 ? mount(target_sub[i], p, NULL, MS_BIND | MS_REC, NULL)
                  : mount("schema-verify", p, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777")) {
            U("cannot mount %s in the target: %m\n", target_sub[i]);
            return;
        }
    }
    if (chroot(tgt) || chdir("/")) {
        U("cannot chroot into the target: %m\n");
        return;
    }

    /* by what it says, not its name: a renamed copy at /sbin/init still gets checked */
    int cap = memfd_create("schema-verify", MFD_CLOEXEC), errfd = memfd_create("schema-verify-err", MFD_CLOEXEC);
    char line[200];
    /* run() would leave the child on our stdout, the report itself */
    if (cap < 0 || errfd < 0) {
        U("cannot capture init=%s's output: memfd_create: %m\n", init);
        return;
    }
    int rc = run((char *[]){ real, "--version", NULL }, cap, cap);
    first_line(cap, line, sizeof line);
    if (rc || strncmp(line, "schema-init ", 12)) {
        if (!strncmp(strrchr(real, '/') + 1, "schema-init", 11))
            F("init=%s --version failed (exit %d)\n", init, rc);
        return;
    }
    dprintf(ctl, "S\n");

    char *argv[8 + MAXWORDS] = { real, "--check", "--root", "/", "--cmdline", (char *)opts };
    int ac = 6;
    char (*xw)[512] = malloc(MAXWORDS * sizeof *xw);
    int nx = extra ? karg_split(extra, xw, MAXWORDS) : 0;
    for (int i = 0; i < nx; i++) argv[ac++] = xw[i];
    argv[ac] = NULL;
    /* through a memfd: schema-bootok's exec= is judged here, inside the target */
    int cfd = memfd_create("schema-verify-check", MFD_CLOEXEC);
    rc = run(argv, cfd >= 0 ? cfd : out, errfd);
    dprintf(ctl, "C %d\n", rc);
    if (cfd >= 0) bootok_exec(ctl, cfd, out);
    first_line(errfd, line, sizeof line);
    if (line[0]) dprintf(ctl, "E %s\n", line);
    /* 1 means error: lines in the output, which the caller judges; anything else is no report */
    if (rc && rc != 1) F("%s --check exited %d%s%s\n", init, rc, line[0] ? ": " : "", line);
}

/* -1 on the deadline: what arrived until then is kept; *over when output was cut */
static int collect(int fds[2], char *bufs[2], size_t lens[2], int *over) {
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct pollfd p[2] = { { fds[0], POLLIN, 0 }, { fds[1], POLLIN, 0 } };
    size_t caps[2] = { 0, 0 };
    int open = 2;
    while (open) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        long left = INSPECT_MS - ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) return -1;
        if (poll(p, 2, left) < 0 && errno != EINTR) return -1;
        for (int i = 0; i < 2; i++) {
            if (p[i].fd < 0 || !p[i].revents) continue;
            if (caps[i] - lens[i] < 4096 && caps[i] < OUT_CAP) {
                caps[i] += 65536;
                bufs[i] = realloc(bufs[i], caps[i] + 1);
            }
            char sink[4096];
            int room = caps[i] - lens[i] >= sizeof sink;
            ssize_t m = read(p[i].fd, room ? bufs[i] + lens[i] : sink, sizeof sink);
            if (m <= 0) {
                p[i].fd = -1;
                open--;
            } else if (room) {
                lens[i] += m;
            } else {
                *over = 1;
            }
        }
    }
    return 0;
}

void root_report_fill(char *ctl, size_t clen, char *out, size_t olen, root_report_t *r) {
    char err[256] = "";
    for (char *l = ctl, *nl; l && l < ctl + clen; l = nl + 1) {
        if (!(nl = memchr(l, '\n', ctl + clen - l))) break;
        *nl = '\0';
        if (l[0] == 'F' && l[1] == ' ') fail(r, "%s", l + 2);
        else if (l[0] == 'U' && l[1] == ' ') unres(r, "%s", l + 2);
        else if (l[0] == 'I' && l[1] == ' ') snprintf(r->init_path, sizeof r->init_path, "%s", l + 2);
        else if (l[0] == 'S') r->is_schema_init = 1;
        else if (l[0] == 'C' && l[1] == ' ') r->check_rc = atoi(l + 2);
        else if (l[0] == 'E' && l[1] == ' ') snprintf(err, sizeof err, "%s", l + 2);
        else if (l[0] == 'B' && l[1] == ' ') r->bootok_exec = atoi(l + 2) == 1 ? 1 : 2;
    }
    static const char hdr[] = "schema-verify-baseline v1\n";
    /* rc 0/1 without the header: an init older than --check, or no report at all */
    if ((r->check_rc == 0 || r->check_rc == 1) && (!out || olen < sizeof hdr - 1 || memcmp(out, hdr, sizeof hdr - 1))) {
        /* the caller decides what an older schema-init means for this entry's role */
        if (r->is_schema_init && r->check_rc == 1 && strstr(err, "unknown option '--check'"))
            r->predates_check = 1;
        else
            unres(r, "%s --check gave no report%s%s", r->init_path, err[0] ? ": " : "", err);
        r->check_rc = -1;
    }
    if (r->check_rc == -1 || !out) {
        free(out);
        return;
    }
    r->check_out = out;
    r->check_len = olen;
}

int root_inspect(const bls_entry_t *e, grubenv_t *env, int nenv, const char *extra, root_report_t *r) {
    char opts[4096];
    if (entry_precheck(e, env, nenv, opts, sizeof opts, r)) goto done;
    if (geteuid() != 0) {
        unres(r, "needs root");
        goto done;
    }
    char base[] = "/tmp/schema-verify-XXXXXX";
    int ctl[2], out[2];
    if (!mkdtemp(base)) {
        unres(r, "cannot inspect the target: mkdtemp: %s", strerror(errno));
        goto done;
    }
    if (pipe2(ctl, O_CLOEXEC)) {
        unres(r, "cannot inspect the target: pipe: %s", strerror(errno));
        rmdir(base);
        goto done;
    }
    if (pipe2(out, O_CLOEXEC)) {
        unres(r, "cannot inspect the target: pipe: %s", strerror(errno));
        close(ctl[0]);
        close(ctl[1]);
        rmdir(base);
        goto done;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(ctl[0]);
        close(out[0]);
        /* its own group, so the deadline kill also takes --version and --check */
        setpgid(0, 0);
        /* no caller fd crosses into the chroot; ctl and out are only written here */
        close_range(3, ~0U, CLOSE_RANGE_CLOEXEC);
        inspect(e, opts, extra, base, ctl[1], out[1]);
        _exit(0);
    }
    close(ctl[1]);
    close(out[1]);
    char *bufs[2] = { NULL, NULL };
    size_t lens[2] = { 0, 0 };
    int st = 0, late = 0, over = 0;
    if (pid < 0) {
        unres(r, "cannot inspect the target: fork: %s", strerror(errno));
    } else {
        setpgid(pid, pid);
        late = collect((int[]){ ctl[0], out[0] }, bufs, lens, &over);
        if (late) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            /* a child stuck in an uninterruptible stat can't be reaped; don't hang with it */
            for (int i = 0; i < 20 && !waitpid(pid, &st, WNOHANG); i++)
                nanosleep(&(struct timespec){ 0, 50000000 }, NULL);
            unres(r, "inspection did not finish within %d s", INSPECT_MS / 1000);
        } else if (waitpid(pid, &st, 0) == pid && !(WIFEXITED(st) && !WEXITSTATUS(st))) {
            unres(r, "inspection ended abnormally (status 0x%x)", st);
        }
    }
    close(ctl[0]);
    close(out[0]);
    rmdir(base);

    if (bufs[1]) bufs[1][lens[1]] = '\0';
    root_report_fill(bufs[0], lens[0], bufs[1], lens[1], r);
    free(bufs[0]);
    /* a cut or late report would read as services gone */
    if ((late || over) && r->check_rc != -1) {
        free(r->check_out);
        r->check_out = NULL;
        r->check_len = 0;
        r->check_rc = -1;
    }
    if (over) unres(r, "--check output over %d MB", OUT_CAP >> 20);
done:
    return r->fail ? 1 : r->nunres ? 3 : 0;
}
