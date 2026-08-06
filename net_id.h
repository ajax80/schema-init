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

#endif /* SCHEMA_NET_ID_H */
