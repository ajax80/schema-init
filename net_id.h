#ifndef SCHEMA_NET_ID_H
#define SCHEMA_NET_ID_H

#include "path_id.h"   /* transitively: schema-udev.h (struct uevent, safe_copy) + pi_* helpers */
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define NID_ARPHRD_ETHER       1
#define NID_ARPHRD_INFINIBAND  32
#define NID_ARPHRD_SLIP        256
#define NID_NAMING_SCHEME      "v259"
#define NID_ONBOARD_INDEX_MAX  ((1U << 14) - 1)

static inline void nid_emit(struct uevent *out, const char *k, const char *v) {
    if (out->n < UE_MAX_KEYS) {
        safe_copy(out->key[out->n], k, UE_KEY_MAX);
        safe_copy(out->val[out->n], v, UE_VAL_MAX);
        out->n++;
    }
}

static inline int nid_uevent_val(const char *devdir, const char *key, char *out, size_t outsz) {
    char p[PATH_MAX];
    if ((size_t)snprintf(p, sizeof p, "%s/uevent", devdir) >= sizeof p) return -1;
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    size_t klen = strlen(key);
    char line[512];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            safe_copy(out, line + klen + 1, outsz);
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

static inline int nid_is_stacked(const char *netdir) {
    char a[64], b[64];
    if (pi_sysattr(netdir, "ifindex", a, sizeof a) != 0) return 0;
    if (pi_sysattr(netdir, "iflink",  b, sizeof b) != 0) return 0;
    return strcmp(a, b) != 0;
}

static inline int nid_arphrd(const char *netdir) {
    char t[64];
    if (pi_sysattr(netdir, "type", t, sizeof t) != 0) return -1;
    return atoi(t);
}

static inline int nid_prefix(const char *netdir, int arphrd, char *out, size_t outsz) {
    if (arphrd == NID_ARPHRD_INFINIBAND) { safe_copy(out, "ib", outsz); return 0; }
    if (arphrd == NID_ARPHRD_SLIP)       { safe_copy(out, "sl", outsz); return 0; }
    if (arphrd == NID_ARPHRD_ETHER) {
        char dt[64];
        if (nid_uevent_val(netdir, "DEVTYPE", dt, sizeof dt) == 0) {
            if (strcmp(dt, "wlan") == 0) { safe_copy(out, "wl", outsz); return 0; }
            if (strcmp(dt, "wwan") == 0) { safe_copy(out, "ww", outsz); return 0; }
        }
        safe_copy(out, "en", outsz);
        return 0;
    }
    return -1;
}

static inline int nid_mac_name(const char *netdir, const char *prefix, int arphrd,
                               char *out, size_t outsz) {
    if (arphrd == NID_ARPHRD_INFINIBAND) return -1;
    char aat[64], alen[64], addr[64];
    if (pi_sysattr(netdir, "addr_assign_type", aat, sizeof aat) != 0) return -1;
    if (atoi(aat) != 0) return -1;                 /* not NET_ADDR_PERM */
    if (pi_sysattr(netdir, "addr_len", alen, sizeof alen) != 0) return -1;
    if (atoi(alen) != 6) return -1;
    if (pi_sysattr(netdir, "address", addr, sizeof addr) != 0) return -1;

    char hex[32]; size_t j = 0;
    for (size_t i = 0; addr[i] && j + 1 < sizeof hex; i++)
        if (addr[i] != ':') hex[j++] = addr[i];
    hex[j] = '\0';
    if (j != 12) return -1;

    snprintf(out, outsz, "%sx%s", prefix, hex);
    return 0;
}

static inline int nid_find_bus_parent(const char *sysroot, const char *devpath,
                                      char *busdir, size_t bussz, char *sub, size_t subsz) {
    char cur[PATH_MAX];
    if ((size_t)snprintf(cur, sizeof cur, "%s%s", sysroot, devpath) >= sizeof cur) return -1;
    if (pi_parent(cur) != 0) return -1;   /* start at the net device's parent */
    for (;;) {
        char s[128];
        if (pi_subsystem(cur, s, sizeof s) == 0 &&
            (strcmp(s, "pci") == 0 || strcmp(s, "usb") == 0 ||
             strcmp(s, "platform") == 0 || strcmp(s, "of") == 0)) {
            safe_copy(busdir, cur, bussz);
            safe_copy(sub, s, subsz);
            return 0;
        }
        if (pi_parent(cur) != 0) return -1;
    }
}

static inline int nid_pci_multifunction(const char *pcidir) {
    char p[PATH_MAX];
    if ((size_t)snprintf(p, sizeof p, "%s/config", pcidir) >= sizeof p) return 0;
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    int multi = 0;
    if (fseek(f, 0x0e, SEEK_SET) == 0) {
        int c = fgetc(f);
        if (c != EOF && (c & 0x80)) multi = 1;
    }
    fclose(f);
    return multi;
}

/* Find a PCI hotplug slot number whose slots/<N>/address is a prefix of dom:bus:slot.
 * Writes the slot dir name to out; 0/-1. */
static inline int nid_pci_slot(const char *sysroot, unsigned dom, unsigned bus, unsigned slot,
                               char *out, size_t outsz) {
    char slotsdir[PATH_MAX];
    if ((size_t)snprintf(slotsdir, sizeof slotsdir, "%s/bus/pci/slots", sysroot) >= sizeof slotsdir)
        return -1;
    DIR *d = opendir(slotsdir);
    if (!d) return -1;
    char want[32];
    snprintf(want, sizeof want, "%04x:%02x:%02x", dom, bus, slot);
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char ad[PATH_MAX], val[64];
        if ((size_t)snprintf(ad, sizeof ad, "%s/%s", slotsdir, e->d_name) >= sizeof ad) continue;
        if (pi_sysattr(ad, "address", val, sizeof val) != 0) continue;
        if (strncmp(val, want, strlen(want)) == 0) { safe_copy(out, e->d_name, outsz); found = 0; break; }
    }
    closedir(d);
    return found;
}

static inline void nid_names_pci(const char *sysroot, const char *pcidir, const char *prefix,
                                 struct uevent *out) {
    unsigned dom, bus, slot, func;
    if (sscanf(pi_base(pcidir), "%x:%x:%x.%x", &dom, &bus, &slot, &func) != 4) return;

    char dp[64]; int dev_port = 0;
    if (pi_sysattr(pcidir, "dev_port", dp, sizeof dp) == 0) dev_port = atoi(dp);
    int multi = nid_pci_multifunction(pcidir);

    /* shared func/dev_port suffix */
    char suffix[32]; size_t so = 0;
    if (func > 0 || multi) so += (size_t)snprintf(suffix + so, sizeof suffix - so, "f%u", func);
    if (dev_port > 0)      so += (size_t)snprintf(suffix + so, sizeof suffix - so, "d%u", dev_port);
    (void)so;

    char domstr[16]; domstr[0] = '\0';
    if (dom > 0) snprintf(domstr, sizeof domstr, "P%u", dom);

    /* ID_NET_NAME_PATH */
    char path[128];
    snprintf(path, sizeof path, "%s%sp%us%u%s", prefix, domstr, bus, slot, suffix);
    nid_emit(out, "ID_NET_NAME_PATH", path);

    /* ID_NET_NAME_SLOT (hotplug) */
    char slotname[64];
    if (nid_pci_slot(sysroot, dom, bus, slot, slotname, sizeof slotname) == 0) {
        char sl[128];
        snprintf(sl, sizeof sl, "%s%ss%s%s", prefix, domstr, slotname, suffix);
        nid_emit(out, "ID_NET_NAME_SLOT", sl);
    }

    /* ID_NET_NAME_ONBOARD (acpi_index preferred, else index) */
    char idxbuf[64]; int idx = 0;
    if (pi_sysattr(pcidir, "acpi_index", idxbuf, sizeof idxbuf) == 0) idx = atoi(idxbuf);
    else if (pi_sysattr(pcidir, "index", idxbuf, sizeof idxbuf) == 0) idx = atoi(idxbuf);
    if (idx > 0 && (unsigned)idx <= NID_ONBOARD_INDEX_MAX) {
        char ob[128];
        snprintf(ob, sizeof ob, "%so%d", prefix, idx);
        nid_emit(out, "ID_NET_NAME_ONBOARD", ob);
    }

    /* ID_NET_LABEL_ONBOARD (firmware label, verbatim) */
    char label[128];
    if (pi_sysattr(pcidir, "label", label, sizeof label) == 0 && label[0])
        nid_emit(out, "ID_NET_LABEL_ONBOARD", label);
}

#endif /* SCHEMA_NET_ID_H */
