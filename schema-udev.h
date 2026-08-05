#ifndef SCHEMA_UDEV_H
#define SCHEMA_UDEV_H

#include <stddef.h>
#include <string.h>

#define UE_MAX_KEYS 32
#define UE_KEY_MAX  64
#define UE_VAL_MAX  512

struct uevent {
    char key[UE_MAX_KEYS][UE_KEY_MAX];
    char val[UE_MAX_KEYS][UE_VAL_MAX];
    int  n;
};

static inline const char *uevent_get(const struct uevent *ev, const char *key) {
    for (int j = 0; j < ev->n; j++)
        if (strcmp(ev->key[j], key) == 0) return ev->val[j];
    return NULL;
}

/* Parse a raw kernel netlink uevent buffer:
 *   "action@devpath\0KEY=VALUE\0KEY=VALUE\0..."
 * The leading action@devpath record is skipped (redundant with ACTION=/DEVPATH=).
 * Returns 0 if >=1 KEY=VALUE parsed and ACTION present, -1 on malformed. */
static inline int uevent_parse(const char *buf, size_t len, struct uevent *ev) {
    ev->n = 0;
    size_t i = 0;
    while (i < len && buf[i] != '\0') i++;   /* skip header record */
    if (i >= len) return -1;                 /* no NUL -> not a uevent */
    i++;
    while (i < len && ev->n < UE_MAX_KEYS) {
        const char *rec = buf + i;
        size_t rl = strnlen(rec, len - i);
        if (rl == 0) { i++; continue; }
        const char *eq = memchr(rec, '=', rl);
        if (eq) {
            size_t klen = (size_t)(eq - rec);
            size_t vlen = rl - klen - 1;
            if (klen > 0 && klen < UE_KEY_MAX && vlen < UE_VAL_MAX) {
                memcpy(ev->key[ev->n], rec, klen); ev->key[ev->n][klen] = '\0';
                memcpy(ev->val[ev->n], eq + 1, vlen); ev->val[ev->n][vlen] = '\0';
                ev->n++;
            }
        }
        i += rl + 1;
    }
    return uevent_get(ev, "ACTION") ? 0 : -1;
}

#endif /* SCHEMA_UDEV_H */
