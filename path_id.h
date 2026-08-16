#ifndef PATH_ID_H
#define PATH_ID_H

#include "schema-udev.h"
#include <limits.h>

#define PATH_ID_MAX 512

static inline int path_id_tag(const char *id_path, char *out, size_t outsz) {
    size_t i = 0;
    for (; id_path[i]; i++) {
        if (i + 1 >= outsz) return -1;
        char c = id_path[i];
        out[i] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-') ? c : '_';
    }
    if (i >= outsz) return -1;
    out[i] = '\0';
    return 0;
}

static inline int pi_subsystem(const char *devdir, char *out, size_t outsz) {
    char link[PATH_MAX], target[PATH_MAX];
    if ((size_t)snprintf(link, sizeof link, "%s/subsystem", devdir) >= sizeof link) return -1;
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n <= 0) return -1;
    target[n] = '\0';
    char *b = strrchr(target, '/');
    safe_copy(out, b ? b + 1 : target, outsz);
    return 0;
}

static inline int pi_driver(const char *devdir, char *out, size_t outsz) {
    char link[PATH_MAX], target[PATH_MAX];
    if ((size_t)snprintf(link, sizeof link, "%s/driver", devdir) >= sizeof link) return -1;
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n <= 0) return -1;
    target[n] = '\0';
    char *b = strrchr(target, '/');
    safe_copy(out, b ? b + 1 : target, outsz);
    return 0;
}

static inline int pi_sysattr(const char *devdir, const char *attr, char *out, size_t outsz) {
    char p[PATH_MAX];
    if ((size_t)snprintf(p, sizeof p, "%s/%s", devdir, attr) >= sizeof p) return -1;
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    if (!fgets(out, (int)outsz, f)) { fclose(f); return -1; }
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    size_t l = strlen(out);
    while (l && (out[l-1] == ' ' || out[l-1] == '\t')) out[--l] = '\0';
    return 0;
}

static inline const char *pi_base(const char *dir) {
    const char *b = strrchr(dir, '/');
    return b ? b + 1 : dir;
}

static inline int pi_parent(char *cur) {
    char *b = strrchr(cur, '/');
    if (!b || b == cur) return -1;
    *b = '\0';
    return 0;
}

static inline void pi_prepend(char *path, size_t pathsz, const char *comp) {
    char tmp[PATH_ID_MAX * 2];
    if (path[0]) snprintf(tmp, sizeof tmp, "%s-%s", comp, path);
    else         snprintf(tmp, sizeof tmp, "%s", comp);
    safe_copy(path, tmp, pathsz);
}

/* ID_PATH_ATA_COMPAT: ID_PATH with the ata "-ata-<port>.<devnum>" reduced to
 * "-ata-<port>" (the pre-devnum backward-compat form). 1 if produced, else 0. */
static inline int pi_ata_compat(const char *idpath, char *out, size_t sz) {
    const char *a = strstr(idpath, "-ata-");
    if (!a) return 0;
    const char *p = a + 5;                       /* past "-ata-" */
    while (*p >= '0' && *p <= '9') p++;           /* port digits */
    if (*p != '.') return 0;
    const char *d = p + 1;
    while (*d >= '0' && *d <= '9') d++;            /* devnum digits */
    size_t head = (size_t)(p - idpath);
    if (head + strlen(d) + 1 > sz) return 0;
    memcpy(out, idpath, head);
    safe_copy(out + head, d, sz - head);          /* drop ".<devnum>" */
    return 1;
}

/* ID_PATH_WITH_USB_REVISION: swap the "usb" token in "-usb-" for "usbv<major>".
 * 1 if the path had a usb token, else 0. */
static inline int pi_usb_rev_swap(const char *idpath, int major, char *out, size_t sz) {
    const char *u = strstr(idpath, "-usb-");
    if (!u) return 0;
    size_t head = (size_t)(u - idpath) + 1;       /* include leading '-', before "usb" */
    int n = snprintf(out, sz, "%.*susbv%d-%s", (int)head, idpath, major, u + 5);
    return (n > 0 && (size_t)n < sz) ? 1 : 0;
}

/* Integer major of the nearest usb-subsystem ancestor's "version" attr, else 0. */
static inline int pi_usb_major(const char *sysroot, const char *devpath) {
    char cur[PATH_MAX];
    if ((size_t)snprintf(cur, sizeof cur, "%s%s", sysroot, devpath) >= sizeof cur) return 0;
    char sub[128], ver[64];
    int maj = 0;
    for (;;) {
        if (pi_subsystem(cur, sub, sizeof sub) == 0 && strcmp(sub, "usb") == 0 &&
            pi_sysattr(cur, "version", ver, sizeof ver) == 0) {
            const char *p = ver; while (*p == ' ' || *p == '\t') p++;
            int m = atoi(p);
            if (m > 0) maj = m;   /* keep climbing: the topmost usb node (root hub) wins */
        }
        if (pi_parent(cur) != 0) break;
        if (strlen(cur) <= strlen(sysroot)) break;
    }
    return maj;
}

static inline int pi_handle_usb(const char *leafdir, char *cur, size_t cursz,
                                char *path, size_t pathsz) {
    (void)leafdir; (void)cursz;
    const char *name = pi_base(cur);
    const char *dash = strchr(name, '-');
    /* usb_device (e.g. 1-7) and usb_interface (e.g. 1-4:1.0) nodes emit usb-0:<rest> */
    if (!dash) return 0;
    char comp[PATH_ID_MAX];
    snprintf(comp, sizeof comp, "usb-0:%s", dash + 1);
    pi_prepend(path, pathsz, comp);
    /* consume: climb past all usb ancestors to the first non-usb parent */
    char sub[128];
    for (;;) {
        if (pi_parent(cur) != 0) break;
        if (pi_subsystem(cur, sub, sizeof sub) != 0 || strcmp(sub, "usb") != 0) break;
    }
    return 1;
}

/* Lowest integer N among sibling dirs named "<prefix>N" inside parent_dir.
 * Returns the min, or -1 if none found. */
static inline int pi_min_index(const char *parent_dir, const char *prefix) {
    DIR *d = opendir(parent_dir);
    if (!d) return -1;
    size_t plen = strlen(prefix);
    int min = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, plen) != 0) continue;
        const char *num = e->d_name + plen;
        if (num[0] < '0' || num[0] > '9') continue;
        int v = atoi(num);
        if (min < 0 || v < min) min = v;
    }
    closedir(d);
    return min;
}

/* Find the ata_device number M (suffix after '.') by scanning
 * <atadir>/link*<any>/dev<N>.<M>. Writes M to out. Returns 0/-1. */
static inline int pi_ata_devnum(const char *atadir, char *out, size_t outsz) {
    DIR *d = opendir(atadir);
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL && found != 0) {
        if (strncmp(e->d_name, "link", 4) != 0) continue;
        char linkdir[PATH_MAX];
        if ((size_t)snprintf(linkdir, sizeof linkdir, "%s/%s", atadir, e->d_name) >= sizeof linkdir)
            continue;
        DIR *ld = opendir(linkdir);
        if (!ld) continue;
        struct dirent *le;
        while ((le = readdir(ld)) != NULL) {
            if (strncmp(le->d_name, "dev", 3) != 0) continue;
            const char *dot = strrchr(le->d_name, '.');
            if (!dot) continue;
            safe_copy(out, dot + 1, outsz);
            found = 0;
            break;
        }
        closedir(ld);
    }
    closedir(d);
    return found;
}

static inline int pi_handle_scsi(const char *leafdir, char *cur, size_t cursz,
                                 char *path, size_t pathsz) {
    (void)leafdir;
    /* only a scsi_device sysname H:C:T:L */
    unsigned H, C, T, L;
    if (sscanf(pi_base(cur), "%u:%u:%u:%u", &H, &C, &T, &L) != 4) return 0;

    /* climb to the hostN dir */
    char hostdir[PATH_MAX];
    safe_copy(hostdir, cur, sizeof hostdir);
    while (strncmp(pi_base(hostdir), "host", 4) != 0) {
        if (pi_parent(hostdir) != 0) return 0;
    }
    /* the dir above hostN: ata port (ata transport) or the plain bus parent */
    char above[PATH_MAX];
    safe_copy(above, hostdir, sizeof above);
    if (pi_parent(above) != 0) return 0;
    const char *abase = pi_base(above);

    char comp[PATH_ID_MAX];
    if (strncmp(abase, "ata", 3) == 0 && abase[3] >= '0' && abase[3] <= '9') {
        /* ATA transport: ata-<port_no>.<M> */
        char port[64], atap[PATH_MAX], devnum[64];
        if ((size_t)snprintf(atap, sizeof atap, "%s/ata_port/%s", above, abase) >= sizeof atap) return 0;
        if (pi_sysattr(atap, "port_no", port, sizeof port) != 0) return 0;
        if (pi_ata_devnum(above, devnum, sizeof devnum) != 0) safe_copy(devnum, "0", sizeof devnum);
        snprintf(comp, sizeof comp, "ata-%s.%s", port, devnum);
        pi_prepend(path, pathsz, comp);
        /* consume up to the ata port's parent */
        safe_copy(cur, above, cursz);
        if (pi_parent(cur) != 0) return 1;
        return 1;
    }

    /* default transport: rebase H by the lowest sibling host index */
    char hostparent[PATH_MAX];
    safe_copy(hostparent, hostdir, sizeof hostparent);
    if (pi_parent(hostparent) != 0) return 0;
    int offset = pi_min_index(hostparent, "host");
    if (offset < 0) offset = 0;
    snprintf(comp, sizeof comp, "scsi-%u:%u:%u:%u", H - (unsigned)offset, C, T, L);
    pi_prepend(path, pathsz, comp);
    /* consume up to the host's parent */
    safe_copy(cur, hostparent, cursz);
    return 1;
}
static inline int pi_handle_nvme(const char *leafdir, char *cur, size_t cursz,
                                 char *path, size_t pathsz) {
    (void)cursz;
    char nsid[64];
    char p[PATH_MAX];
    safe_copy(p, leafdir, sizeof p);
    while (pi_sysattr(p, "nsid", nsid, sizeof nsid) != 0) {
        if (strcmp(p, cur) == 0 || pi_parent(p) != 0) return 0;
    }
    char comp[PATH_ID_MAX];
    snprintf(comp, sizeof comp, "nvme-%s", nsid);
    pi_prepend(path, pathsz, comp);
    if (pi_parent(cur) != 0) return 1;
    return 1;
}

static inline ssize_t path_id_build(const char *sysroot, const char *devpath,
                                    char *out, size_t outsz) {
    char devroot[PATH_MAX], cur[PATH_MAX], leafdir[PATH_MAX];
    if ((size_t)snprintf(devroot, sizeof devroot, "%s/devices", sysroot) >= sizeof devroot)
        return -1;
    if ((size_t)snprintf(cur, sizeof cur, "%s%s", sysroot, devpath) >= sizeof cur)
        return -1;
    safe_copy(leafdir, cur, sizeof leafdir);

    size_t rootlen = strlen(devroot);
    char path[PATH_ID_MAX] = "";
    int anchored = 0;
    char comp[PATH_ID_MAX], sub[128];

    while (strlen(cur) > rootlen) {
        if (pi_subsystem(cur, sub, sizeof sub) != 0) {
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "pci") == 0) {
            snprintf(comp, sizeof comp, "pci-%s", pi_base(cur));
            pi_prepend(path, sizeof path, comp);
            anchored = 1;
            /* skip all pci ancestors (bridges) */
            for (;;) {
                if (pi_parent(cur) != 0 || strlen(cur) <= rootlen) break;
                if (pi_subsystem(cur, sub, sizeof sub) != 0 || strcmp(sub, "pci") != 0) break;
            }
            continue;
        }
        if (strcmp(sub, "platform") == 0) {
            snprintf(comp, sizeof comp, "platform-%s", pi_base(cur));
            pi_prepend(path, sizeof path, comp);
            anchored = 1;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "usb") == 0) {
            if (pi_handle_usb(leafdir, cur, sizeof cur, path, sizeof path)) continue;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "scsi") == 0) {
            if (pi_handle_scsi(leafdir, cur, sizeof cur, path, sizeof path)) continue;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (strcmp(sub, "nvme") == 0) {
            if (pi_handle_nvme(leafdir, cur, sizeof cur, path, sizeof path)) continue;
            if (pi_parent(cur) != 0) break;
            continue;
        }
        if (pi_parent(cur) != 0) break;
    }

    if (!anchored || path[0] == '\0') return -1;
    size_t plen = strlen(path);
    if (plen + 1 > outsz) return -1;
    memcpy(out, path, plen + 1);
    return (ssize_t)plen;
}

#endif /* PATH_ID_H */
