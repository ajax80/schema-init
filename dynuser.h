#ifndef SCHEMA_DYNUSER_H
#define SCHEMA_DYNUSER_H

#include <stdint.h>
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

#endif
