#include "verify_disk.h"
#include "verify_root.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int has_opt(const char *opts, const char *name) {
    size_t l = strlen(name);
    for (const char *p = opts; p && *p;) {
        size_t n = strcspn(p, ",");
        if (n >= l && !strncmp(p, name, l) && (n == l || p[l] == '=')) return 1;
        p += n + (p[n] == ',');
    }
    return 0;
}

static int in_list(const char *s, const char *const *l) {
    for (; *l; l++)
        if (!strcmp(s, *l)) return 1;
    return 0;
}

static int is_devspec(const char *s) {
    return !strncmp(s, "UUID=", 5) || !strncmp(s, "PARTUUID=", 9) || !strncmp(s, "LABEL=", 6) ||
           !strncmp(s, "PARTLABEL=", 10) || !strncmp(s, "/dev/", 5);
}

fsclass_t fstab_class(const struct mntent *m) {
    static const char *const net[] = { "nfs", "nfs4", "cifs", "smb3", "smbfs", "sshfs", "9p", "ceph",
                                       "glusterfs", "afs", "davfs", "lustre", "ocfs2", NULL };
    static const char *const virt[] = { "tmpfs", "ramfs", "proc", "sysfs", "devpts", "devtmpfs", "cgroup",
                                        "cgroup2", "overlay", "securityfs", "debugfs", "tracefs", "configfs",
                                        "fusectl", "pstore", "bpf", "mqueue", "hugetlbfs", "efivarfs", NULL };
    const char *src = m->mnt_fsname, *type = m->mnt_type, *o = m->mnt_opts;
    if (has_opt(o, "noauto")) return FS_NOAUTO;
    if (!strcmp(type, "swap")) return FS_SWAP;
    if (has_opt(o, "bind") || has_opt(o, "rbind")) return FS_BIND;
    if (!strncmp(type, "fuse", 4)) return FS_LAYERED;
    if (in_list(type, net)) return FS_NETWORK;
    if (in_list(type, virt)) return FS_VIRTUAL;
    if (!strncmp(src, "/dev/mapper/", 12)) return FS_LAYERED;
    if (!is_devspec(src)) return FS_VIRTUAL;
    /* nofail decides it even with an automount or a device timeout: the boot never waits on it */
    if (has_opt(o, "nofail")) return FS_LOCAL_NOFAIL;
    if (has_opt(o, "x-systemd.automount")) return FS_AUTOMOUNT;
    return FS_LOCAL_REQ;
}

static int add(finding_t *out, int max, int *n, fkind_t k, const char *fmt, ...) {
    if (*n < max) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(out[*n].text, sizeof out[*n].text, fmt, ap);
        va_end(ap);
        out[*n].kind = k;
    }
    return (*n)++;
}

/* getmntent leaves blkid-style quotes in: UUID="x" and "UUID=x" are UUID=x */
static void unquote(char *s) {
    size_t l = strlen(s);
    char *v = s[0] == '"' ? s : strchr(s, '=');
    /* an opening and a closing quote, two characters: UUID=" alone is left as it is */
    if (!v || l < 2 || s[l - 1] != '"' || (v != s && (v[1] != '"' || strlen(v + 1) < 2))) return;
    if (v != s) v++;
    memmove(v, v + 1, strlen(v + 1) - 1);
    s[l - 2] = '\0';
}

static void fstab_findings(const char *path, const char *devdir, finding_t *out, int max, int *n) {
    FILE *f = setmntent(path, "r");
    if (!f) return;
    struct mntent *m;
    char dev[PATH_MAX];
    while ((m = getmntent(f))) {
        unquote(m->mnt_fsname);
        const char *src = m->mnt_fsname, *dir = m->mnt_dir, *t = m->mnt_type;
        fsclass_t c = fstab_class(m);
        int missing = 0;
        if (c == FS_LOCAL_REQ || c == FS_LOCAL_NOFAIL || c == FS_SWAP)
            missing = is_devspec(src) ? dev_resolve(src, devdir, dev, sizeof dev) : access(src, F_OK);
        switch (c) {
        case FS_LOCAL_REQ:
            if (missing) add(out, max, n, F_FAIL, "%s \xe2\x80\x94 required %s device %s not present", dir, t, src);
            break;
        case FS_LOCAL_NOFAIL:
            if (missing) add(out, max, n, F_WARN, "%s \xe2\x80\x94 nofail mount is currently unavailable (%s not present)", dir, src);
            break;
        case FS_SWAP:
            if (missing) add(out, max, n, F_WARN, "swap %s not present", src);
            break;
        case FS_NETWORK: add(out, max, n, F_NOTE, "%s \xe2\x80\x94 network filesystem; device presence not checked", dir); break;
        case FS_VIRTUAL: add(out, max, n, F_NOTE, "%s \xe2\x80\x94 virtual filesystem; no device to check", dir); break;
        case FS_BIND: add(out, max, n, F_NOTE, "%s \xe2\x80\x94 bind mount; device presence not checked", dir); break;
        case FS_AUTOMOUNT: add(out, max, n, F_NOTE, "%s \xe2\x80\x94 automount; device presence not checked", dir); break;
        case FS_LAYERED: add(out, max, n, F_NOTE, "%s \xe2\x80\x94 layered or virtual source (%s); device presence not checked", dir, t); break;
        case FS_NOAUTO: add(out, max, n, F_NOTE, "%s \xe2\x80\x94 noauto; not mounted at boot", dir); break;
        }
    }
    endmntent(f);
}

/* sde1 -> sde, nvme0n1p2 -> nvme0n1, mmcblk0p1 -> mmcblk0: errors on a partition are the disk's */
static void whole_disk(char *d) {
    size_t l = strlen(d), e = l;
    while (e && d[e - 1] >= '0' && d[e - 1] <= '9') e--;
    if (e == l || !e) return;
    if (e > 1 && d[e - 1] == 'p' && d[e - 2] >= '0' && d[e - 2] <= '9') d[e - 1] = '\0';
    else if (!strncmp(d, "sd", 2) || !strncmp(d, "hd", 2) || !strncmp(d, "vd", 2) || !strncmp(d, "xvd", 3)) d[e] = '\0';
}

#define MAXDEV 32
static void count(char (*names)[32], int (*cnt)[2], int *nd, const char *dev, size_t dl, int fs) {
    char d[32];
    if (!dl || dl >= sizeof d) return;
    /* optical, loop and zram: nothing the boot waits on */
    if (!strncmp(dev, "sr", 2) || !strncmp(dev, "loop", 4) || !strncmp(dev, "zram", 4)) return;
    memcpy(d, dev, dl);
    d[dl] = '\0';
    whole_disk(d);
    int i;
    for (i = 0; i < *nd && strcmp(names[i], d); i++)
        ;
    if (i == *nd) {
        if (*nd == MAXDEV) return;
        strcpy(names[(*nd)++], d);
        cnt[i][0] = cnt[i][1] = 0;
    }
    cnt[i][fs]++;
}

static size_t dev_token(const char *s) {
    size_t n = 0;
    while ((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '-' || s[n] == '_') n++;
    return n;
}

static void kmsg_findings(const char *text, finding_t *out, int max, int *n) {
    char names[MAXDEV][32];
    int cnt[MAXDEV][2], nd = 0;
    for (const char *l = text; l && *l;) {
        const char *nl = strchr(l, '\n');
        size_t len = nl ? (size_t)(nl - l) : strlen(l);
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)len, l);
        l = nl ? nl + 1 : NULL;
        const char *d;
        if (strstr(line, "I/O error") && (d = strstr(line, "dev "))) {
            count(names, cnt, &nd, d + 4, dev_token(d + 4), 0);
        } else if (strstr(line, "EXT4-fs error") && (d = strstr(line, "(device "))) {
            count(names, cnt, &nd, d + 8, dev_token(d + 8), 1);
        } else if (strstr(line, "BTRFS") && !strstr(line, "BTRFS info") &&
                   (strstr(line, "corrupt") || strstr(line, "errs:")) && (d = strstr(line, "(device "))) {
            /* not info: at mount btrfs prints its persisted lifetime errs: counters
               at info level; an error since boot logs as BTRFS error/warning */
            count(names, cnt, &nd, d + 8, dev_token(d + 8), 1);
        }
    }
    for (int i = 0; i < nd; i++) {
        int t = cnt[i][0] + cnt[i][1];
        add(out, max, n, F_WARN, "%s \xe2\x80\x94 %d %s error%s in the kernel log since boot", names[i], t,
            cnt[i][1] ? "I/O or filesystem" : "I/O", t == 1 ? "" : "s");
    }
}

int disk_findings(const char *fstab_path, const char *kmsg_text, const char *devdir, finding_t *out, int max) {
    int n = 0;
    fstab_findings(fstab_path, devdir, out, max, &n);
    if (kmsg_text) kmsg_findings(kmsg_text, out, max, &n);
    return n;
}
