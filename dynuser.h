#ifndef SCHEMA_DYNUSER_H
#define SCHEMA_DYNUSER_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* systemd's dynamic uid range */
#define DYNUSER_MIN 61184u
#define DYNUSER_MAX 65519u

/* The uid for dynamic_user=1: hashed from the name, so a service gets the
 * same one every boot unless something took it, then probed upward with
 * wraparound past whatever `taken` reports. 0 when the range is full. */
static inline uid_t dynuser_pick(const char *name, int (*taken)(uid_t, void *), void *ctx) {
    uint32_t h = 2166136261u, span = DYNUSER_MAX - DYNUSER_MIN + 1;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (uint32_t i = 0; i < span; i++) {
        uid_t u = (uid_t)(DYNUSER_MIN + (h % span + i) % span);
        if (!taken(u, ctx)) return u;
    }
    return 0;
}

#define DYNUSER_MAP_BYTES ((DYNUSER_MAX - DYNUSER_MIN + 1 + 7) / 8)

static inline int dynuser_mapped(const uint8_t *map, uid_t u) {
    if (u < DYNUSER_MIN || u > DYNUSER_MAX) return 0;
    u -= DYNUSER_MIN;
    return map[u / 8] >> (u % 8) & 1;
}

/* mark the ids in field `field` (0-based, ':'-separated) of a passwd/group
 * file that fall in the range. The local files, read once per allocation:
 * an NSS lookup per probe could block PID 1's loop on LDAP/sssd. */
static inline void dynuser_scan(const char *path, int field, uint8_t *map) {
    FILE *f = fopen(path, "re");
    char line[1024];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        for (int i = 0; i < field && p; i++) p = strchr(p, ':') ? strchr(p, ':') + 1 : NULL;
        if (!p || *p < '0' || *p > '9') continue;
        unsigned long v = strtoul(p, NULL, 10);
        if (v >= DYNUSER_MIN && v <= DYNUSER_MAX) {
            v -= DYNUSER_MIN;
            map[v / 8] |= (uint8_t)(1u << (v % 8));
        }
    }
    fclose(f);
}

#endif
